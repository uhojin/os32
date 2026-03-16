#include "spotify_client.h"
#include "spotify_auth.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp32s3/rom/tjpgd.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "spotify_client";

static constexpr const char *API_BASE = "https://api.spotify.com/v1/me/player";
static constexpr size_t RESP_BUF_SIZE = 16384;

namespace os32 {

// ---------------------------------------------------------------------------
// JPEG from memory (same TJpgDec pattern as thumbnail.cpp but from RAM)
// ---------------------------------------------------------------------------

struct MemJpgCtx {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint16_t *output;
    int out_w;
};

static UINT mem_jpg_input(JDEC *jd, BYTE *buf, UINT ndata)
{
    auto *ctx = static_cast<MemJpgCtx *>(jd->device);
    UINT avail = ctx->size - ctx->pos;
    if (ndata > avail) ndata = avail;
    if (buf)
        memcpy(buf, ctx->data + ctx->pos, ndata);
    ctx->pos += ndata;
    return ndata;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static UINT mem_jpg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    auto *ctx = static_cast<MemJpgCtx *>(jd->device);
    auto *src = static_cast<uint8_t *>(bitmap);

    for (WORD y = rect->top; y <= rect->bottom; y++) {
        for (WORD x = rect->left; x <= rect->right; x++) {
            uint8_t r = *src++;
            uint8_t g = *src++;
            uint8_t b = *src++;
            ctx->output[y * ctx->out_w + x] = rgb565(r, g, b);
        }
    }
    return 1;
}

static Thumbnail decode_jpg_from_mem(const uint8_t *data, size_t len,
                                     int max_w, int max_h)
{
    MemJpgCtx ctx = {};
    ctx.data = data;
    ctx.size = len;
    ctx.pos = 0;

    auto *work = static_cast<BYTE *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!work) return {};
    JDEC jd;

    if (jd_prepare(&jd, mem_jpg_input, work, 4096, &ctx) != JDR_OK) {
        heap_caps_free(work);
        return {};
    }

    int src_w = jd.width;
    int src_h = jd.height;

    // Use TJpgDec hardware scaling to get close to target
    int jd_scale = 0;
    for (int s = 3; s >= 0; s--) {
        if ((src_w >> s) >= max_w && (src_h >> s) >= max_h) {
            jd_scale = s;
            break;
        }
    }

    int dec_w = src_w >> jd_scale;
    int dec_h = src_h >> jd_scale;

    ctx.output = static_cast<uint16_t *>(
        heap_caps_malloc(dec_w * dec_h * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!ctx.output) { heap_caps_free(work); return {}; }
    ctx.out_w = dec_w;

    if (jd_decomp(&jd, mem_jpg_output, static_cast<BYTE>(jd_scale)) != JDR_OK) {
        heap_caps_free(ctx.output);
        heap_caps_free(work);
        return {};
    }
    heap_caps_free(work);

    // Scale-to-fill and center-crop to max_w x max_h
    // Use the smaller scale factor so the image fills the target area
    float sx = static_cast<float>(dec_w) / max_w;
    float sy = static_cast<float>(dec_h) / max_h;
    float scale = (sx < sy) ? sx : sy;  // fill, not fit

    int tw = max_w;
    int th = max_h;
    // Source region to sample from (centered crop)
    int crop_w = static_cast<int>(tw * scale);
    int crop_h = static_cast<int>(th * scale);
    int crop_x = (dec_w - crop_w) / 2;
    int crop_y = (dec_h - crop_h) / 2;

    auto *out = static_cast<uint16_t *>(
        heap_caps_malloc(tw * th * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (out) {
        for (int dy = 0; dy < th; dy++) {
            int srow = crop_y + dy * crop_h / th;
            for (int dx = 0; dx < tw; dx++) {
                int scol = crop_x + dx * crop_w / tw;
                out[dy * tw + dx] = ctx.output[srow * dec_w + scol];
            }
        }
        heap_caps_free(ctx.output);
        return {out, tw, th};
    }

    return {ctx.output, dec_w, dec_h};
}

// ---------------------------------------------------------------------------
// HTTP client helper
// ---------------------------------------------------------------------------

struct HttpRespCtx {
    char *buf;
    size_t capacity;
    size_t len;
};

static esp_err_t client_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto *ctx = static_cast<HttpRespCtx *>(evt->user_data);
        size_t avail = ctx->capacity - ctx->len - 1;
        size_t copy = static_cast<size_t>(evt->data_len);
        if (copy > avail) copy = avail;
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

void SpotifyClient::init(SpotifyAuth *auth)
{
    auth_ = auth;
}

int SpotifyClient::do_api_call(const char *method, const char *url,
                               const char *body, char *resp, size_t resp_size)
{
    char token[512];
    auth_->get_access_token(token, sizeof(token));
    if (!token[0]) return -1;

    char auth_header[560];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);

    HttpRespCtx resp_ctx = {};
    resp_ctx.buf = resp;
    resp_ctx.capacity = resp_size;
    resp_ctx.len = 0;
    resp[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = client_event_handler;
    config.user_data = &resp_ctx;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    auto client = esp_http_client_init(&config);

    // Set method
    if (strcmp(method, "PUT") == 0)
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    else if (strcmp(method, "POST") == 0)
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    else
        esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_http_client_set_header(client, "Authorization", auth_header);

    if (body && body[0]) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    return status;
}

// ---------------------------------------------------------------------------
// JSON parsing
// ---------------------------------------------------------------------------

bool SpotifyClient::parse_currently_playing(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    memset(&result_track_, 0, sizeof(result_track_));

    const cJSON *is_playing = cJSON_GetObjectItem(root, "is_playing");
    result_track_.is_playing = cJSON_IsTrue(is_playing);

    const cJSON *progress = cJSON_GetObjectItem(root, "progress_ms");
    if (cJSON_IsNumber(progress))
        result_track_.progress_ms = static_cast<uint32_t>(progress->valuedouble);

    const cJSON *item = cJSON_GetObjectItem(root, "item");
    if (!cJSON_IsObject(item)) {
        cJSON_Delete(root);
        return false;
    }

    // Track name
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    if (cJSON_IsString(name))
        strncpy(result_track_.title, name->valuestring, sizeof(result_track_.title) - 1);

    // Duration
    const cJSON *duration = cJSON_GetObjectItem(item, "duration_ms");
    if (cJSON_IsNumber(duration))
        result_track_.duration_ms = static_cast<uint32_t>(duration->valuedouble);

    // First artist
    const cJSON *artists = cJSON_GetObjectItem(item, "artists");
    if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
        const cJSON *first = cJSON_GetArrayItem(artists, 0);
        const cJSON *aname = cJSON_GetObjectItem(first, "name");
        if (cJSON_IsString(aname))
            strncpy(result_track_.artist, aname->valuestring, sizeof(result_track_.artist) - 1);
    }

    // Album name and ID
    const cJSON *album = cJSON_GetObjectItem(item, "album");
    if (cJSON_IsObject(album)) {
        const cJSON *alname = cJSON_GetObjectItem(album, "name");
        if (cJSON_IsString(alname))
            strncpy(result_track_.album, alname->valuestring, sizeof(result_track_.album) - 1);

        const cJSON *alid = cJSON_GetObjectItem(album, "id");
        if (cJSON_IsString(alid))
            strncpy(result_track_.album_id, alid->valuestring, sizeof(result_track_.album_id) - 1);
    }

    // Extract art URL from item.album.images (avoids separate album API call)
    const cJSON *images = cJSON_GetObjectItem(album, "images");
    if (cJSON_IsArray(images)) {
        int count = cJSON_GetArraySize(images);
        int best_idx = 0;
        int best_w = 0;
        for (int i = 0; i < count; i++) {
            cJSON *img = cJSON_GetArrayItem(images, i);
            cJSON *w = cJSON_GetObjectItem(img, "width");
            int width = cJSON_IsNumber(w) ? (int)w->valuedouble : 0;
            if (width >= 120 && (best_w < 120 || width < best_w)) {
                best_w = width;
                best_idx = i;
            } else if (best_w < 120 && width > best_w) {
                best_w = width;
                best_idx = i;
            }
        }
        cJSON *img = cJSON_GetArrayItem(images, best_idx);
        cJSON *url = cJSON_GetObjectItem(img, "url");
        if (cJSON_IsString(url))
            strncpy(result_track_.art_url, url->valuestring, sizeof(result_track_.art_url) - 1);
    }

    result_track_.valid = true;
    cJSON_Delete(root);
    return true;
}

// ---------------------------------------------------------------------------
// Album art download + decode
// ---------------------------------------------------------------------------

struct ArtDownloadCtx {
    uint8_t *buf;
    size_t capacity;
    size_t len;
};

static esp_err_t art_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto *ctx = static_cast<ArtDownloadCtx *>(evt->user_data);
        size_t avail = ctx->capacity - ctx->len;
        size_t copy = static_cast<size_t>(evt->data_len);
        if (copy > avail) copy = avail;
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    return ESP_OK;
}

Thumbnail SpotifyClient::fetch_and_decode_art(const char *url, int max_w, int max_h)
{
    constexpr size_t ART_BUF_SIZE = 131072;
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(ART_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!buf) return {};

    ArtDownloadCtx dl = {};
    dl.buf = buf;
    dl.capacity = ART_BUF_SIZE;
    dl.len = 0;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = art_event_handler;
    config.user_data = &dl;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    auto client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || dl.len == 0) {
        ESP_LOGE(TAG, "Art download failed: err=%d status=%d len=%u",
                 err, status, (unsigned)dl.len);
        heap_caps_free(buf);
        return {};
    }

    ESP_LOGI(TAG, "Art downloaded: %u bytes", (unsigned)dl.len);
    Thumbnail result = decode_jpg_from_mem(buf, dl.len, max_w, max_h);
    heap_caps_free(buf);
    return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SpotifyClient::post_command(SpotifyCmd cmd)
{
    pending_cmd_ = cmd;
    if (state_ != SpotifyClientState::BUSY) {
        state_ = SpotifyClientState::IDLE;
        poll();
    }
}

void SpotifyClient::poll()
{
    if (state_ == SpotifyClientState::BUSY) return;
    state_ = SpotifyClientState::BUSY;
    xTaskCreatePinnedToCore(task_func, "spotify", 8192, this, 5, nullptr, 1);
}

SpotifyTrack SpotifyClient::take_track()
{
    SpotifyTrack t = result_track_;
    if (state_ == SpotifyClientState::DONE_TRACK)
        state_ = SpotifyClientState::IDLE;
    return t;
}

Thumbnail SpotifyClient::take_art()
{
    Thumbnail t = result_art_;
    result_art_ = {};
    if (state_ == SpotifyClientState::DONE_ART)
        state_ = SpotifyClientState::IDLE;
    return t;
}

// ---------------------------------------------------------------------------
// Background task (runs on core 1)
// ---------------------------------------------------------------------------

void SpotifyClient::task_func(void *arg)
{
    auto *self = static_cast<SpotifyClient *>(arg);

    auto *resp = static_cast<char *>(heap_caps_calloc(1, RESP_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!resp) {
        self->state_ = SpotifyClientState::ERROR;
        vTaskDelete(nullptr);
        return;
    }

    // Proactively refresh token if expired (avoids noisy 401 round-trip)
    if (self->auth_->token_expired()) {
        if (!self->auth_->refresh_token_sync()) {
            self->state_ = SpotifyClientState::AUTH_EXPIRED;
            heap_caps_free(resp);
            vTaskDelete(nullptr);
            return;
        }
    }

    // Handle pending command first
    SpotifyCmd cmd = self->pending_cmd_;
    self->pending_cmd_ = SpotifyCmd::NONE;

    if (cmd != SpotifyCmd::NONE && cmd != SpotifyCmd::POLL_NOW) {
        char url[128];
        const char *method = "PUT";
        const char *body = nullptr;

        switch (cmd) {
            case SpotifyCmd::PLAY:
                snprintf(url, sizeof(url), "%s/play", API_BASE);
                method = "PUT";
                break;
            case SpotifyCmd::PAUSE:
                snprintf(url, sizeof(url), "%s/pause", API_BASE);
                method = "PUT";
                break;
            case SpotifyCmd::NEXT:
                snprintf(url, sizeof(url), "%s/next", API_BASE);
                method = "POST";
                break;
            case SpotifyCmd::PREV:
                snprintf(url, sizeof(url), "%s/previous", API_BASE);
                method = "POST";
                break;
            case SpotifyCmd::VOLUME_UP:
                self->volume_ = (self->volume_ + 5 <= 100) ? self->volume_ + 5 : 100;
                snprintf(url, sizeof(url), "%s/volume?volume_percent=%d",
                         API_BASE, self->volume_ & 0x7F);
                method = "PUT";
                break;
            case SpotifyCmd::VOLUME_DOWN:
                self->volume_ = (self->volume_ - 5 >= 0) ? self->volume_ - 5 : 0;
                snprintf(url, sizeof(url), "%s/volume?volume_percent=%d",
                         API_BASE, self->volume_ & 0x7F);
                method = "PUT";
                break;
            default:
                break;
        }

        int status = self->do_api_call(method, url, body, resp, RESP_BUF_SIZE);

        if (status == 401) {
            // Token expired, try refresh
            if (self->auth_->refresh_token_sync()) {
                status = self->do_api_call(method, url, body, resp, RESP_BUF_SIZE);
            }
        }

        if (status == 401) {
            self->state_ = SpotifyClientState::AUTH_EXPIRED;
            heap_caps_free(resp);
            vTaskDelete(nullptr);
            return;
        }

        if (status == 404) {
            self->state_ = SpotifyClientState::NO_DEVICE;
            heap_caps_free(resp);
            vTaskDelete(nullptr);
            return;
        }

        // After command, poll current state
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Poll currently playing
    char url[128];
    snprintf(url, sizeof(url), "%s/currently-playing", API_BASE);
    resp[0] = '\0';

    int status = self->do_api_call("GET", url, nullptr, resp, RESP_BUF_SIZE);

    if (status == 401) {
        if (self->auth_->refresh_token_sync()) {
            resp[0] = '\0';
            status = self->do_api_call("GET", url, nullptr, resp, RESP_BUF_SIZE);
        }
    }

    if (status == 401) {
        self->state_ = SpotifyClientState::AUTH_EXPIRED;
        heap_caps_free(resp);
        vTaskDelete(nullptr);
        return;
    }

    if (status == 204 || status == -1) {
        // Nothing playing or error
        memset(&self->result_track_, 0, sizeof(self->result_track_));
        self->state_ = (status == 204) ? SpotifyClientState::DONE_TRACK
                                        : SpotifyClientState::ERROR;
        heap_caps_free(resp);
        vTaskDelete(nullptr);
        return;
    }

    if (status == 200 && self->parse_currently_playing(resp)) {
        // Check if album changed
        bool album_changed = self->result_track_.album_id[0] &&
            strcmp(self->result_track_.album_id, self->last_album_id_) != 0;

        if (album_changed) {
            heap_caps_free(resp);

            if (self->result_track_.art_url[0]) {
                ESP_LOGI(TAG, "Album art URL: %s", self->result_track_.art_url);
                self->result_art_.release();
                self->result_art_ = self->fetch_and_decode_art(
                    self->result_track_.art_url, 120, 120);

                if (self->result_art_.valid()) {
                    strncpy(self->last_album_id_, self->result_track_.album_id,
                            sizeof(self->last_album_id_) - 1);
                    self->state_ = SpotifyClientState::DONE_ART;
                } else {
                    ESP_LOGW(TAG, "Art decode failed");
                    self->state_ = SpotifyClientState::DONE_TRACK;
                }
            } else {
                ESP_LOGW(TAG, "No art URL in track data");
                self->state_ = SpotifyClientState::DONE_TRACK;
            }
        } else {
            heap_caps_free(resp);
            self->state_ = SpotifyClientState::DONE_TRACK;
        }
    } else {
        heap_caps_free(resp);
        self->state_ = SpotifyClientState::ERROR;
    }

    vTaskDelete(nullptr);
}

} // namespace os32
