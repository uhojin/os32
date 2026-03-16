#include "screenshot.h"
#include "os32.h"
#include "sd_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "screenshot";

namespace os32 {

static int scan_next_index()
{
    int next = 0;
    DIR *dir = opendir("/sdcard/DCIM");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        int num = 0;
        if (sscanf(ent->d_name, "SCR_%d.bmp", &num) == 1) {
            if (num >= next) next = num + 1;
        }
    }
    closedir(dir);
    return next;
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

bool screenshot_save(SdManager *sd)
{
    if (!sd || !sd->mounted()) {
        ESP_LOGW(TAG, "No SD card");
        return false;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) return false;

    // Allocate snapshot buffer in PSRAM (LVGL's internal pool is too small)
    uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, LV_COLOR_FORMAT_RGB565);
    uint32_t data_size = stride * LCD_V_RES;
    uint8_t *px_data = static_cast<uint8_t *>(heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM));
    if (!px_data) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%lu bytes)", (unsigned long)data_size);
        return false;
    }

    lv_draw_buf_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.header.w = LCD_H_RES;
    snap.header.h = LCD_V_RES;
    snap.header.cf = LV_COLOR_FORMAT_RGB565;
    snap.header.stride = stride;
    snap.data = px_data;
    snap.data_size = data_size;

    lv_result_t res = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, &snap);
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Snapshot render failed");
        heap_caps_free(px_data);
        return false;
    }

    int w = LCD_H_RES;
    int h = LCD_V_RES;

    // BMP file structure
    constexpr uint32_t HEADER_SIZE = 14 + 40;  // file header + info header
    uint32_t row_bytes = w * 3;
    uint32_t row_pad = (4 - (row_bytes % 4)) % 4;
    uint32_t bmp_stride = row_bytes + row_pad;
    uint32_t pixel_size = bmp_stride * h;
    uint32_t file_size = HEADER_SIZE + pixel_size;

    mkdir("/sdcard/DCIM", 0755);

    int idx = scan_next_index();
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/DCIM/SCR_%04d.bmp", idx & 0xFFFF);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", path);
        heap_caps_free(px_data);
        return false;
    }

    // Write BMP file header (14 bytes)
    uint8_t hdr[HEADER_SIZE] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    write_le32(&hdr[2], file_size);
    // reserved = 0 (already zeroed)
    write_le32(&hdr[10], HEADER_SIZE);

    // Write BMP info header (40 bytes)
    write_le32(&hdr[14], 40);           // header size
    write_le32(&hdr[18], w);            // width
    write_le32(&hdr[22], h);            // height (positive = bottom-up)
    write_le16(&hdr[26], 1);            // planes
    write_le16(&hdr[28], 24);           // bits per pixel
    // compression = 0 (BI_RGB), rest = 0

    fwrite(hdr, 1, HEADER_SIZE, f);

    // Write pixel data row by row, bottom-up (BMP format)
    uint8_t row_buf[LCD_H_RES * 3];
    uint8_t pad[3] = {};

    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src_row = snap.data + y * stride;

        for (int x = 0; x < w; x++) {
            uint16_t px = src_row[x * 2] | (src_row[x * 2 + 1] << 8);

            // RGB565 → RGB888 (BMP stores as BGR)
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;

            row_buf[x * 3 + 0] = (b5 << 3) | (b5 >> 2);  // B
            row_buf[x * 3 + 1] = (g6 << 2) | (g6 >> 4);  // G
            row_buf[x * 3 + 2] = (r5 << 3) | (r5 >> 2);  // R
        }

        fwrite(row_buf, 1, row_bytes, f);
        if (row_pad > 0) fwrite(pad, 1, row_pad, f);
    }

    fclose(f);
    heap_caps_free(px_data);

    ESP_LOGI(TAG, "Saved %s (%lux%lu, %lu bytes)", path,
             (unsigned long)w, (unsigned long)h, (unsigned long)file_size);
    sd->notify_change();
    return true;
}

} // namespace os32
