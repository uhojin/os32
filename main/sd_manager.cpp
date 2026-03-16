#include "sd_manager.h"
#include "os32.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "ff.h"
#include <cstring>

static const char *TAG = "sd";

namespace os32 {

bool SdManager::init()
{
    if (mounted_) return true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0  = SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount_cfg, &card_);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount FAT filesystem (not formatted?)");
        } else {
            ESP_LOGW(TAG, "SD card init failed: %s", esp_err_to_name(err));
        }
        card_ = nullptr;
        return false;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT);
    sdmmc_card_print_info(stdout, card_);
    return true;
}

void SdManager::deinit()
{
    if (mounted_ && card_) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, card_);
        card_ = nullptr;
        mounted_ = false;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}

const char* SdManager::mount_point() const
{
    return SD_MOUNT;
}

uint64_t SdManager::total_bytes() const
{
    if (!mounted_ || !card_) return 0;
    return (uint64_t)card_->csd.capacity * card_->csd.sector_size;
}

uint64_t SdManager::free_bytes() const
{
    if (!mounted_) return 0;
    FATFS *fs;
    DWORD free_clust;
    if (f_getfree("0:", &free_clust, &fs) != FR_OK) return 0;
    return (uint64_t)free_clust * fs->csize * fs->ssize;
}

} // namespace os32
