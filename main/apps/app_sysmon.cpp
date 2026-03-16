#include "app_sysmon.h"
#include "os32.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include <cstdio>
#include "ff.h"

namespace os32 {

void SysMonApp::on_enter(lv_obj_t *screen)
{
    info_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(info_label_, font_mono(), 0);
    lv_obj_set_style_text_color(info_label_, color::fg(), 0);
    lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
    lv_label_set_text(info_label_, "Loading...");

    refresh_accum_ = 500; // trigger immediate first update
}

void SysMonApp::on_update(uint32_t dt_ms)
{
    refresh_accum_ += dt_ms;
    if (refresh_accum_ < 500) return;
    refresh_accum_ = 0;

    // Gather stats
    unsigned free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
    unsigned min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) / 1024;
    unsigned free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024;

    int64_t uptime_us = esp_timer_get_time();
    unsigned up_sec = static_cast<unsigned>(uptime_us / 1000000);
    unsigned up_h = up_sec / 3600;
    unsigned up_m = (up_sec / 60) % 60;
    unsigned up_s = up_sec % 60;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    // SD card info
    FATFS *fs;
    DWORD free_clust;
    bool sd_ok = (f_getfree("0:", &free_clust, &fs) == FR_OK);
    unsigned sd_total_mb = 0, sd_free_mb = 0;
    if (sd_ok) {
        sd_total_mb = (uint64_t)(fs->n_fatent - 2) * fs->csize * fs->ssize / 1048576;
        sd_free_mb  = (uint64_t)free_clust * fs->csize * fs->ssize / 1048576;
    }

    char sd_info[32];
    if (sd_ok) {
        snprintf(sd_info, sizeof(sd_info), "SD %u/%uMB", sd_free_mb, sd_total_mb);
    } else {
        snprintf(sd_info, sizeof(sd_info), "SD --");
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
        "ESP32-S3 r%d.%d\n"
        "Cores %d\n"
        "Flash %s\n"
        "\n"
        "Heap %u KB free\n"
        "Low  %u KB\n"
        "DMA  %u KB free\n"
        "\n"
        "%s\n"
        "\n"
        "Up %02u:%02u:%02u",
        chip.revision / 100, chip.revision % 100,
        chip.cores,
        (chip.features & CHIP_FEATURE_EMB_FLASH) ? "emb" : "ext",
        free_heap, min_heap, free_dma,
        sd_info,
        up_h, up_m, up_s
    );

    lv_label_set_text(info_label_, buf);
}

void SysMonApp::on_exit()
{
    info_label_ = nullptr;
}

bool SysMonApp::on_button(ButtonId btn, ButtonEvent event)
{
    return false;
}

void SysMonApp::get_status_text(char *line1, char *line2)
{
    constexpr int NUM_PAGES = 4;
    status_accum_ += 500;  // called every 500ms from main loop
    if (status_accum_ >= 2000) {
        status_accum_ = 0;
        status_page_ = (status_page_ + 1) % NUM_PAGES;
    }

    switch (status_page_) {
    case 0: {
        unsigned free_kb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024;
        unsigned min_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) / 1024;
        snprintf(line1, 17, "Heap  %4uKB", free_kb);
        snprintf(line2, 17, "Low   %4uKB", min_kb);
        break;
    }
    case 1: {
        unsigned free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024;
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(line1, 17, "DMA   %4uKB", free_dma);
        snprintf(line2, 17, "S3 r%u.%u  %uc", chip.revision / 100, chip.revision % 100, chip.cores);
        break;
    }
    case 2: {
        FATFS *sfs;
        DWORD sfc;
        if (f_getfree("0:", &sfc, &sfs) == FR_OK) {
            unsigned total = (uint64_t)(sfs->n_fatent - 2) * sfs->csize * sfs->ssize / 1048576;
            unsigned free_mb = (uint64_t)sfc * sfs->csize * sfs->ssize / 1048576;
            snprintf(line1, 17, "SD %4uMB", total);
            snprintf(line2, 17, "Free %4uMB", free_mb);
        } else {
            snprintf(line1, 17, "SD Card");
            snprintf(line2, 17, "Not mounted");
        }
        break;
    }
    case 3: {
        int64_t uptime_us = esp_timer_get_time();
        unsigned up_sec = static_cast<unsigned>(uptime_us / 1000000);
        unsigned h = (up_sec / 3600) % 100;
        unsigned m = (up_sec / 60) % 60;
        unsigned s = up_sec % 60;
        snprintf(line1, 17, "Uptime");
        snprintf(line2, 17, "  %02u:%02u:%02u", h, m, s);
        break;
    }
    }
}

} // namespace os32
