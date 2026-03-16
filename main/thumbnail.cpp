#include "thumbnail.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp32s3/rom/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "thumb";

namespace os32 {

void Thumbnail::release()
{
    if (pixels) {
        heap_caps_free(pixels);
        pixels = nullptr;
    }
    width = height = 0;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static uint32_t read_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

// Common nearest-neighbor downsample from an intermediate RGBA or RGB565 buffer
static Thumbnail downsample_rgb565(uint16_t *src, int src_w, int src_h,
                                   int max_w, int max_h)
{
    if (src_w <= max_w && src_h <= max_h)
        return {src, src_w, src_h};

    float sx = static_cast<float>(src_w) / max_w;
    float sy = static_cast<float>(src_h) / max_h;
    float scale = (sx > sy) ? sx : sy;

    int tw = static_cast<int>(src_w / scale);
    int th = static_cast<int>(src_h / scale);
    if (tw <= 0) tw = 1;
    if (th <= 0) th = 1;

    auto *out = static_cast<uint16_t *>(
        heap_caps_malloc(tw * th * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!out) return {src, src_w, src_h};

    for (int dy = 0; dy < th; dy++) {
        int sy_row = dy * src_h / th;
        for (int dx = 0; dx < tw; dx++) {
            int sx_col = dx * src_w / tw;
            out[dy * tw + dx] = src[sy_row * src_w + sx_col];
        }
    }

    heap_caps_free(src);
    return {out, tw, th};
}

// ---------------------------------------------------------------------------
// BMP decoder — 24-bit uncompressed only
// ---------------------------------------------------------------------------

static Thumbnail decode_bmp(const char *path, int max_w, int max_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return {};

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(f);
        return {};
    }

    uint32_t px_offset = read_le32(&hdr[10]);
    int32_t w = static_cast<int32_t>(read_le32(&hdr[18]));
    int32_t h = static_cast<int32_t>(read_le32(&hdr[22]));
    uint16_t bpp = read_le16(&hdr[28]);
    uint16_t compression = read_le16(&hdr[30]);

    bool bottom_up = (h > 0);
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || bpp != 24 || compression != 0) {
        fclose(f);
        return {};
    }

    float sx = static_cast<float>(w) / max_w;
    float sy = static_cast<float>(h) / max_h;
    float scale = (sx > sy) ? sx : sy;
    if (scale < 1.0f) scale = 1.0f;

    int tw = static_cast<int>(w / scale);
    int th = static_cast<int>(h / scale);
    if (tw <= 0 || th <= 0) { fclose(f); return {}; }

    auto *out = static_cast<uint16_t *>(
        heap_caps_malloc(tw * th * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!out) { fclose(f); return {}; }

    int src_row_bytes = ((w * 3) + 3) & ~3;
    auto *row_buf = static_cast<uint8_t *>(malloc(src_row_bytes));
    if (!row_buf) { heap_caps_free(out); fclose(f); return {}; }

    for (int dy = 0; dy < th; dy++) {
        int src_y = static_cast<int>(dy * scale);
        if (src_y >= h) src_y = h - 1;
        int file_row = bottom_up ? (h - 1 - src_y) : src_y;

        fseek(f, px_offset + static_cast<long>(file_row) * src_row_bytes, SEEK_SET);
        fread(row_buf, 1, src_row_bytes, f);

        for (int dx = 0; dx < tw; dx++) {
            int src_x = static_cast<int>(dx * scale);
            if (src_x >= w) src_x = w - 1;
            out[dy * tw + dx] = rgb565(
                row_buf[src_x * 3 + 2],
                row_buf[src_x * 3 + 1],
                row_buf[src_x * 3 + 0]);
        }
    }

    free(row_buf);
    fclose(f);
    ESP_LOGI(TAG, "BMP %ldx%ld -> %dx%d", (long)w, (long)h, tw, th);
    return {out, tw, th};
}

// ---------------------------------------------------------------------------
// JPEG decoder — ROM TJpgDec (RGB888 output → convert to RGB565)
// ---------------------------------------------------------------------------

struct JpgCtx {
    FILE *file;
    uint16_t *output;
    int out_w;
};

static UINT jpg_input(JDEC *jd, BYTE *buf, UINT ndata)
{
    auto *ctx = static_cast<JpgCtx *>(jd->device);
    if (buf)
        return static_cast<UINT>(fread(buf, 1, ndata, ctx->file));
    fseek(ctx->file, ndata, SEEK_CUR);
    return ndata;
}

static UINT jpg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    auto *ctx = static_cast<JpgCtx *>(jd->device);
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

static Thumbnail decode_jpg(const char *path, int max_w, int max_h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return {};

    JpgCtx ctx = {};
    ctx.file = f;

    auto *work = static_cast<BYTE *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!work) { fclose(f); return {}; }
    JDEC jd;

    if (jd_prepare(&jd, jpg_input, work, 4096, &ctx) != JDR_OK) {
        heap_caps_free(work);
        fclose(f);
        return {};
    }

    int src_w = jd.width;
    int src_h = jd.height;

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
    if (!ctx.output) { fclose(f); return {}; }
    ctx.out_w = dec_w;

    if (jd_decomp(&jd, jpg_output, static_cast<BYTE>(jd_scale)) != JDR_OK) {
        heap_caps_free(ctx.output);
        heap_caps_free(work);
        fclose(f);
        return {};
    }
    heap_caps_free(work);
    fclose(f);

    ESP_LOGI(TAG, "JPG %dx%d -> %dx%d (1/%d)", src_w, src_h, dec_w, dec_h, 1 << jd_scale);
    return downsample_rgb565(ctx.output, dec_w, dec_h, max_w, max_h);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Thumbnail thumbnail_load(const char *path, int max_w, int max_h)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return {};
    dot++;

    if (strcasecmp(dot, "bmp") == 0)
        return decode_bmp(path, max_w, max_h);
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0)
        return decode_jpg(path, max_w, max_h);

    return {};
}

// ---------------------------------------------------------------------------
// ThumbLoader — async decode on core 1
// ---------------------------------------------------------------------------

void ThumbLoader::start(const char *path, int max_w, int max_h)
{
    cancel_ = true;
    while (state_ == State::LOADING)
        vTaskDelay(pdMS_TO_TICKS(5));

    result_.release();

    strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';
    max_w_ = max_w;
    max_h_ = max_h;
    cancel_ = false;
    state_ = State::LOADING;

    xTaskCreatePinnedToCore(task_func, "thumb", 4096, this, 5, nullptr, 1);
}

void ThumbLoader::cancel()
{
    cancel_ = true;
}

Thumbnail ThumbLoader::take_result()
{
    Thumbnail t = result_;
    result_ = {};
    state_ = State::IDLE;
    return t;
}

void ThumbLoader::task_func(void *arg)
{
    auto *self = static_cast<ThumbLoader *>(arg);
    Thumbnail result = thumbnail_load(self->path_, self->max_w_, self->max_h_);

    if (self->cancel_) {
        result.release();
        self->state_ = State::IDLE;
    } else {
        self->result_ = result;
        self->state_ = result.valid() ? State::DONE : State::FAILED;
    }

    vTaskDelete(nullptr);
}

} // namespace os32
