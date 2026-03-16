#include "spotify_auth.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "spotify_auth";

// Configure via menuconfig: os32 Configuration > Spotify
static constexpr const char *CLIENT_ID = CONFIG_SPOTIFY_CLIENT_ID;
static constexpr const char *REDIRECT_URI = CONFIG_SPOTIFY_REDIRECT_URI;
static constexpr const char *SCOPES = "user-read-playback-state "
                                       "user-modify-playback-state "
                                       "user-read-currently-playing";
static constexpr int AUTH_PORT = 8888;
static constexpr const char *NVS_NS = "spotify";
static constexpr const char *NVS_KEY_ACCESS = "access_tok";
static constexpr const char *NVS_KEY_REFRESH = "refresh_tok";

namespace os32 {

SpotifyAuth *SpotifyAuth::s_instance_ = nullptr;

// ---------------------------------------------------------------------------
// Base64url encoding (no padding)
// ---------------------------------------------------------------------------

static size_t base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    size_t out = 0;
    for (size_t i = 0; i < src_len && out < dst_len - 1; i += 3) {
        uint32_t n = static_cast<uint32_t>(src[i]) << 16;
        if (i + 1 < src_len) n |= static_cast<uint32_t>(src[i + 1]) << 8;
        if (i + 2 < src_len) n |= src[i + 2];

        dst[out++] = table[(n >> 18) & 0x3F];
        dst[out++] = table[(n >> 12) & 0x3F];
        if (i + 1 < src_len) dst[out++] = table[(n >> 6) & 0x3F];
        if (i + 2 < src_len) dst[out++] = table[n & 0x3F];
    }
    dst[out] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// URL encoding
// ---------------------------------------------------------------------------

static size_t url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (; *src && out + 3 < dst_len; src++) {
        char c = *src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = c;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0xF];
            dst[out++] = hex[c & 0xF];
        }
    }
    dst[out] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// PKCE
// ---------------------------------------------------------------------------

void SpotifyAuth::generate_pkce()
{
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

    // Generate 43-char code verifier
    for (int i = 0; i < 43; i++)
        code_verifier_[i] = charset[esp_random() % (sizeof(charset) - 1)];
    code_verifier_[43] = '\0';

    // SHA256(verifier) -> base64url = code challenge
    uint8_t hash[32];
    mbedtls_sha256(reinterpret_cast<const uint8_t *>(code_verifier_),
                   43, hash, 0);
    base64url_encode(hash, 32, code_challenge_, sizeof(code_challenge_));

    ESP_LOGI(TAG, "PKCE verifier generated, challenge: %.8s...", code_challenge_);
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

void SpotifyAuth::load_tokens()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;

    size_t alen = sizeof(tokens_.access_token);
    size_t rlen = sizeof(tokens_.refresh_token);
    bool ok = (nvs_get_str(nvs, NVS_KEY_ACCESS, tokens_.access_token, &alen) == ESP_OK)
           && (nvs_get_str(nvs, NVS_KEY_REFRESH, tokens_.refresh_token, &rlen) == ESP_OK);
    nvs_close(nvs);

    if (ok && tokens_.refresh_token[0]) {
        state_ = SpotifyAuthState::TOKEN_VALID;
        tokens_.expires_at = 0;  // unknown, will refresh on first use if needed
        ESP_LOGI(TAG, "Loaded tokens from NVS");
    }
}

void SpotifyAuth::save_tokens()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_ACCESS, tokens_.access_token);
    nvs_set_str(nvs, NVS_KEY_REFRESH, tokens_.refresh_token);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Tokens saved to NVS");
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void SpotifyAuth::init()
{
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    s_instance_ = this;
    load_tokens();
}

SpotifyAuthState SpotifyAuth::state() const
{
    return state_;
}

bool SpotifyAuth::tokens_valid() const
{
    return state_ == SpotifyAuthState::TOKEN_VALID && tokens_.access_token[0];
}

bool SpotifyAuth::token_expired() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool expired = tokens_.expires_at == 0 ||
                   esp_timer_get_time() >= tokens_.expires_at;
    xSemaphoreGive(mutex_);
    return expired;
}

void SpotifyAuth::get_access_token(char *buf, size_t len) const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(buf, tokens_.access_token, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(mutex_);
}

void SpotifyAuth::forget()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    memset(&tokens_, 0, sizeof(tokens_));
    state_ = SpotifyAuthState::NOT_CONFIGURED;
    xSemaphoreGive(mutex_);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Credentials forgotten");
}

// ---------------------------------------------------------------------------
// Auth URL
// ---------------------------------------------------------------------------

void SpotifyAuth::get_auth_url(char *buf, size_t len) const
{
    char scopes_enc[128];
    url_encode(SCOPES, scopes_enc, sizeof(scopes_enc));

    char redirect_enc[256];
    url_encode(REDIRECT_URI, redirect_enc, sizeof(redirect_enc));

    snprintf(buf, len,
        "https://accounts.spotify.com/authorize"
        "?client_id=%s"
        "&response_type=code"
        "&redirect_uri=%s"
        "&code_challenge_method=S256"
        "&code_challenge=%s"
        "&scope=%s",
        CLIENT_ID, redirect_enc, code_challenge_, scopes_enc);
}

// ---------------------------------------------------------------------------
// Token exchange
// ---------------------------------------------------------------------------

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        auto *buf = static_cast<char *>(evt->user_data);
        size_t cur = strlen(buf);
        size_t avail = 2048 - cur;
        if (static_cast<size_t>(evt->data_len) < avail) {
            memcpy(buf + cur, evt->data, evt->data_len);
            buf[cur + evt->data_len] = '\0';
        }
    }
    return ESP_OK;
}

bool SpotifyAuth::exchange_code(const char *code)
{
    char *resp = static_cast<char *>(heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM));
    if (!resp) return false;

    char *post = static_cast<char *>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
    if (!post) { heap_caps_free(resp); return false; }

    char redirect_enc[256];
    url_encode(REDIRECT_URI, redirect_enc, sizeof(redirect_enc));

    snprintf(post, 1024,
        "grant_type=authorization_code"
        "&code=%s"
        "&redirect_uri=%s"
        "&client_id=%s"
        "&code_verifier=%s",
        code, redirect_enc, CLIENT_ID, code_verifier_);

    esp_http_client_config_t config = {};
    config.url = "https://accounts.spotify.com/api/token";
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = resp;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    auto client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post, strlen(post));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    heap_caps_free(post);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Token exchange failed: err=%d status=%d", err, status);
        ESP_LOGE(TAG, "Response: %.200s", resp);
        heap_caps_free(resp);
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);
    if (!root) return false;

    const cJSON *at = cJSON_GetObjectItem(root, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    const cJSON *ei = cJSON_GetObjectItem(root, "expires_in");

    if (!cJSON_IsString(at) || !cJSON_IsString(rt)) {
        cJSON_Delete(root);
        return false;
    }

    int expires_in = cJSON_IsNumber(ei) ? ei->valueint : 3600;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(tokens_.access_token, at->valuestring, sizeof(tokens_.access_token) - 1);
    strncpy(tokens_.refresh_token, rt->valuestring, sizeof(tokens_.refresh_token) - 1);
    tokens_.expires_at = esp_timer_get_time() + static_cast<int64_t>(expires_in) * 1000000LL;
    state_ = SpotifyAuthState::TOKEN_VALID;
    xSemaphoreGive(mutex_);

    save_tokens();
    ESP_LOGI(TAG, "Token exchange successful, expires in %ds", expires_in);
    return true;
}

bool SpotifyAuth::refresh_token_sync()
{
    char refresh[512];
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(refresh, tokens_.refresh_token, sizeof(refresh) - 1);
    refresh[sizeof(refresh) - 1] = '\0';
    xSemaphoreGive(mutex_);

    if (!refresh[0]) return false;

    char *resp = static_cast<char *>(heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM));
    if (!resp) return false;

    char post[768];
    snprintf(post, sizeof(post),
        "grant_type=refresh_token"
        "&refresh_token=%s"
        "&client_id=%s",
        refresh, CLIENT_ID);

    esp_http_client_config_t config = {};
    config.url = "https://accounts.spotify.com/api/token";
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = resp;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    auto client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post, strlen(post));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Token refresh failed: err=%d status=%d", err, status);
        heap_caps_free(resp);
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);
    if (!root) return false;

    const cJSON *at = cJSON_GetObjectItem(root, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    const cJSON *ei = cJSON_GetObjectItem(root, "expires_in");

    if (!cJSON_IsString(at)) {
        cJSON_Delete(root);
        return false;
    }

    int expires_in = cJSON_IsNumber(ei) ? ei->valueint : 3600;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(tokens_.access_token, at->valuestring, sizeof(tokens_.access_token) - 1);
    // Spotify may or may not return a new refresh token
    if (cJSON_IsString(rt))
        strncpy(tokens_.refresh_token, rt->valuestring, sizeof(tokens_.refresh_token) - 1);
    tokens_.expires_at = esp_timer_get_time() + static_cast<int64_t>(expires_in) * 1000000LL;
    state_ = SpotifyAuthState::TOKEN_VALID;
    xSemaphoreGive(mutex_);

    save_tokens();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Token refreshed, expires in %ds", expires_in);
    return true;
}

void SpotifyAuth::update_tokens(const char *access, const char *refresh, int expires_in_sec)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(tokens_.access_token, access, sizeof(tokens_.access_token) - 1);
    if (refresh && refresh[0])
        strncpy(tokens_.refresh_token, refresh, sizeof(tokens_.refresh_token) - 1);
    tokens_.expires_at = esp_timer_get_time() + static_cast<int64_t>(expires_in_sec) * 1000000LL;
    state_ = SpotifyAuthState::TOKEN_VALID;
    xSemaphoreGive(mutex_);
    save_tokens();
}

// ---------------------------------------------------------------------------
// HTTP handlers for auth flow
// ---------------------------------------------------------------------------

static const char AUTH_PAGE_HTML[] = R"rawhtml(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>os32 Spotify</title>
<style>
body{background:#282828;color:#ebdbb2;font-family:monospace;display:flex;
justify-content:center;align-items:center;height:100vh;margin:0}
.box{text-align:center;padding:2em}
h1{color:#fabd2f;margin-bottom:1em}
a{display:inline-block;background:#1DB954;color:#000;padding:12px 24px;
text-decoration:none;border-radius:4px;font-weight:bold;font-size:1.1em}
a:hover{background:#1ed760}
</style></head><body>
<div class="box"><h1>os32</h1>
<a href="%s">Login with Spotify</a>
</div></body></html>
)rawhtml";

static const char AUTH_SUCCESS_HTML[] = R"rawhtml(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>os32 Spotify</title>
<style>
body{background:#282828;color:#ebdbb2;font-family:monospace;display:flex;
justify-content:center;align-items:center;height:100vh;margin:0}
h1{color:#b8bb26}
</style></head><body>
<div style="text-align:center"><h1>Connected!</h1><p>You can close this tab.</p></div>
</body></html>
)rawhtml";

static const char AUTH_ERROR_HTML[] = R"rawhtml(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>os32 Spotify</title>
<style>
body{background:#282828;color:#ebdbb2;font-family:monospace;display:flex;
justify-content:center;align-items:center;height:100vh;margin:0}
h1{color:#fb4934}
</style></head><body>
<div style="text-align:center"><h1>Error</h1><p>Authorization failed. Try again from the device.</p></div>
</body></html>
)rawhtml";

esp_err_t SpotifyAuth::handle_root(httpd_req_t *req)
{
    auto *self = s_instance_;
    if (!self) return ESP_FAIL;

    char *url = static_cast<char *>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
    if (!url) return ESP_ERR_NO_MEM;
    self->get_auth_url(url, 1024);

    char *page = static_cast<char *>(heap_caps_malloc(2048, MALLOC_CAP_SPIRAM));
    if (!page) { heap_caps_free(url); return ESP_ERR_NO_MEM; }
    snprintf(page, 2048, AUTH_PAGE_HTML, url);
    heap_caps_free(url);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, strlen(page));
    heap_caps_free(page);
    return ESP_OK;
}

esp_err_t SpotifyAuth::handle_callback(httpd_req_t *req)
{
    auto *self = s_instance_;
    if (!self) return ESP_FAIL;

    // Extract ?code= from query string
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 512) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
        return ESP_OK;
    }

    char *query = static_cast<char *>(malloc(qlen + 1));
    if (!query) return ESP_ERR_NO_MEM;
    httpd_req_get_url_query_str(req, query, qlen + 1);

    char code[256] = {};
    esp_err_t err = httpd_query_key_value(query, "code", code, sizeof(code));
    free(query);

    if (err != ESP_OK || !code[0]) {
        ESP_LOGE(TAG, "No code in callback");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Got auth code, exchanging for tokens...");

    if (self->exchange_code(code)) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_SUCCESS_HTML, strlen(AUTH_SUCCESS_HTML));
    } else {
        self->state_ = SpotifyAuthState::ERROR;
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
    }

    return ESP_OK;
}

// Handler for /spotify-code — receives the code from the GitHub Pages relay
esp_err_t SpotifyAuth::handle_spotify_code(httpd_req_t *req)
{
    auto *self = s_instance_;
    if (!self) return ESP_FAIL;

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 1024) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
        return ESP_OK;
    }

    char *query = static_cast<char *>(malloc(qlen + 1));
    if (!query) return ESP_ERR_NO_MEM;
    httpd_req_get_url_query_str(req, query, qlen + 1);

    char code[512] = {};
    httpd_query_key_value(query, "code", code, sizeof(code));
    free(query);

    if (!code[0]) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Got auth code from relay, exchanging for tokens...");

    if (self->exchange_code(code)) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_SUCCESS_HTML, strlen(AUTH_SUCCESS_HTML));
    } else {
        self->state_ = SpotifyAuthState::ERROR;
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, AUTH_ERROR_HTML, strlen(AUTH_ERROR_HTML));
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Auth flow lifecycle
// ---------------------------------------------------------------------------

void SpotifyAuth::start_auth_flow(WifiManager *wifi)
{
    wifi_ = wifi;
    generate_pkce();

    // Start HTTP server for relay code reception
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = AUTH_PORT;
    config.stack_size = 8192;
    config.max_uri_len = 1024;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start auth server");
        state_ = SpotifyAuthState::ERROR;
        return;
    }

    // Root page with Spotify login link (fallback if user visits device directly)
    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = handle_root;
    httpd_register_uri_handler(server_, &root_uri);

    // Direct callback (kept for local-network fallback)
    httpd_uri_t cb_uri = {};
    cb_uri.uri = "/callback";
    cb_uri.method = HTTP_GET;
    cb_uri.handler = handle_callback;
    httpd_register_uri_handler(server_, &cb_uri);

    // Code relay endpoint — GitHub Pages relay redirects here
    httpd_uri_t code_uri = {};
    code_uri.uri = "/spotify-code";
    code_uri.method = HTTP_GET;
    code_uri.handler = handle_spotify_code;
    httpd_register_uri_handler(server_, &code_uri);

    state_ = SpotifyAuthState::AUTHORIZING;
    ESP_LOGI(TAG, "Auth server started on port %d", AUTH_PORT);
}

void SpotifyAuth::stop_auth_flow()
{
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
        ESP_LOGI(TAG, "Auth server stopped");
    }
    if (state_ == SpotifyAuthState::AUTHORIZING)
        state_ = SpotifyAuthState::NOT_CONFIGURED;
}

} // namespace os32
