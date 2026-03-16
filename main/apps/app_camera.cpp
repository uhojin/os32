#include "app_camera.h"
#include "os32.h"
#include "sd_manager.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "camera";

static constexpr int CAM_W = 320;
static constexpr int CAM_H = 240;

static const char *FILTER_NAMES[] = {
    "Normal", "Negative", "Grayscale", "Red", "Green", "Blue", "Sepia"
};

namespace os32 {

static camera_config_t make_cam_config()
{
    camera_config_t cfg = {};
    cfg.pin_pwdn = -1;
    cfg.pin_reset = -1;
    cfg.pin_xclk = 15;
    cfg.pin_sccb_sda = 4;
    cfg.pin_sccb_scl = 5;
    cfg.pin_d7 = 16;
    cfg.pin_d6 = 17;
    cfg.pin_d5 = 18;
    cfg.pin_d4 = 12;
    cfg.pin_d3 = 10;
    cfg.pin_d2 = 8;
    cfg.pin_d1 = 9;
    cfg.pin_d0 = 11;
    cfg.pin_vsync = 6;
    cfg.pin_href = 7;
    cfg.pin_pclk = 13;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.fb_count = 2;  // double buffer for pipelining
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    return cfg;
}

bool CameraApp::init_camera()
{
    auto cfg = make_cam_config();
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size = FRAMESIZE_QVGA;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
    }

    // Note: GPIO 48 = LCD SPI SCLK — do NOT touch it here.
    // The onboard LED on GPIO 48 is driven by the SPI clock and cannot be
    // independently controlled without killing the display.

    ESP_LOGI(TAG, "Camera initialized (direct render)");
    return true;
}

void CameraApp::scan_existing_snapshots()
{
    snap_next_ = 0;
    snap_count_ = 0;
    DIR *dir = opendir("/sdcard/DCIM");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        int num = 0;
        if (sscanf(ent->d_name, "IMG_%d.jpg", &num) == 1) {
            snap_count_++;
            if (num >= snap_next_) snap_next_ = num + 1;
        }
    }
    closedir(dir);
    if (snap_next_ > 0) {
        ESP_LOGI(TAG, "Resuming at IMG_%04d.jpg (%d existing)",
                 snap_next_ & 0xFFFF, snap_count_ & 0xFF);
    }
}

void CameraApp::on_enter(lv_obj_t *screen)
{
    snap_msg_timer_ = 0;
    filter_ = 0;
    scan_existing_snapshots();
    cam_ready_ = init_camera();

    if (!cam_ready_) {
        // Fall back to LVGL for error message
        auto *lbl = lv_label_create(screen);
        lv_obj_set_style_text_font(lbl, font_mono(), 0);
        lv_obj_set_style_text_color(lbl, color::red(), 0);
        lv_obj_set_pos(lbl, PAD, CONTENT_Y);
        lv_label_set_text(lbl, "Camera not found\n\nCheck FPC cable");
    }
}

// Ping-pong DMA buffers — SPI reads from one while we fill the other
// 320px * 10 rows * 2 bytes = 6400 bytes each, 24 strips per frame
static constexpr int STRIP_ROWS = 10;
static constexpr size_t STRIP_SIZE = CAM_W * STRIP_ROWS * 2;
static uint8_t *s_dma_buf0 = nullptr;
static uint8_t *s_dma_buf1 = nullptr;
static int s_dma_idx = 0;

void CameraApp::push_frame()
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    // Allocate ping-pong DMA buffers on first use
    if (!s_dma_buf0) {
        s_dma_buf0 = static_cast<uint8_t*>(heap_caps_malloc(STRIP_SIZE, MALLOC_CAP_DMA));
        s_dma_buf1 = static_cast<uint8_t*>(heap_caps_malloc(STRIP_SIZE, MALLOC_CAP_DMA));
        if (!s_dma_buf0 || !s_dma_buf1) {
            ESP_LOGE(TAG, "DMA buf alloc failed");
            heap_caps_free(s_dma_buf0);
            heap_caps_free(s_dma_buf1);
            s_dma_buf0 = s_dma_buf1 = nullptr;
            esp_camera_fb_return(fb);
            cam_ready_ = false;
            return;
        }
    }

    const uint8_t *src = fb->buf;
    for (int y = 0; y < CAM_H; y += STRIP_ROWS) {
        int rows = (y + STRIP_ROWS <= CAM_H) ? STRIP_ROWS : (CAM_H - y);
        size_t chunk = CAM_W * rows * 2;

        uint8_t *buf = (s_dma_idx == 0) ? s_dma_buf0 : s_dma_buf1;
        memcpy(buf, src, chunk);

        esp_lcd_panel_draw_bitmap(lcd_panel(), 0, y, CAM_W, y + rows, buf);

        s_dma_idx ^= 1;
        src += chunk;
    }

    esp_camera_fb_return(fb);
}

void CameraApp::save_snapshot()
{
    if (!sd_ || !sd_->mounted()) {
        ESP_LOGW(TAG, "No SD card");
        return;
    }

    esp_camera_deinit();

    auto cfg = make_cam_config();
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = FRAMESIZE_VGA;
    cfg.jpeg_quality = 10;
    cfg.fb_count = 1;

    if (esp_camera_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "JPEG init failed");
        // Restore preview
        cfg.pixel_format = PIXFORMAT_RGB565;
        cfg.frame_size = FRAMESIZE_QVGA;
        cfg.fb_count = 2;
        esp_camera_init(&cfg);
        sensor_t *s = esp_camera_sensor_get();
        if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }
    apply_filter();

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb && fb->len > 0) {
        mkdir("/sdcard/DCIM", 0755);

        char path[64];
        snprintf(path, sizeof(path), "/sdcard/DCIM/IMG_%04d.jpg",
                 snap_next_ & 0xFFFF);

        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(fb->buf, 1, fb->len, f);
            fclose(f);
            snap_next_++;
            snap_count_++;
            snap_msg_timer_ = 2000;
            ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)fb->len);
        }
        esp_camera_fb_return(fb);
    }

    // Restore preview
    esp_camera_deinit();
    cfg.pixel_format = PIXFORMAT_RGB565;
    cfg.frame_size = FRAMESIZE_QVGA;
    cfg.fb_count = 2;
    esp_camera_init(&cfg);
    s = esp_camera_sensor_get();
    if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 1); }
    apply_filter();
}

void CameraApp::on_update(uint32_t dt_ms)
{
    if (!cam_ready_) return;

    if (snap_msg_timer_ > 0) {
        snap_msg_timer_ = (dt_ms >= snap_msg_timer_) ? 0 : snap_msg_timer_ - dt_ms;
    }

    push_frame();
}

void CameraApp::on_exit()
{
    if (cam_ready_) {
        esp_camera_deinit();
        cam_ready_ = false;
    }
    heap_caps_free(s_dma_buf0);
    heap_caps_free(s_dma_buf1);
    s_dma_buf0 = s_dma_buf1 = nullptr;
}

bool CameraApp::on_button(ButtonId btn, ButtonEvent event)
{
    if (event != ButtonEvent::PRESS) return false;

    switch (btn) {
    case ButtonId::RIGHT:
        if (cam_ready_) save_snapshot();
        return true;
    case ButtonId::UP:
        if (cam_ready_) {
            filter_ = (filter_ + 1) % FILTER_COUNT;
            apply_filter();
        }
        return true;
    case ButtonId::DOWN:
        if (cam_ready_) {
            filter_ = (filter_ - 1 + FILTER_COUNT) % FILTER_COUNT;
            apply_filter();
        }
        return true;
    default:
        return false;
    }
}

void CameraApp::apply_filter()
{
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_special_effect(s, filter_);
}

void CameraApp::get_status_text(char *line1, char *line2)
{
    snprintf(line1, 17, "%-10s%5s", "Camera", FILTER_NAMES[filter_]);
    if (snap_msg_timer_ > 0) {
        snprintf(line2, 17, "Saved #%04d", (snap_next_ - 1) & 0xFFFF);
    } else if (!cam_ready_) {
        snprintf(line2, 17, "No camera");
    } else {
        snprintf(line2, 17, "Snaps: %d", snap_count_ & 0xFF);
    }
}

} // namespace os32
