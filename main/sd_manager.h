#pragma once

#include "sdmmc_cmd.h"
#include <cstdint>

namespace os32 {

class SdManager {
public:
    bool init();
    void deinit();

    bool mounted() const { return mounted_; }
    uint64_t total_bytes() const;
    uint64_t free_bytes() const;
    const char* mount_point() const;

    void notify_change() { files_changed_ = true; }
    bool consume_change() { bool c = files_changed_; files_changed_ = false; return c; }

private:
    sdmmc_card_t *card_ = nullptr;
    bool mounted_ = false;
    volatile bool files_changed_ = false;
};

} // namespace os32
