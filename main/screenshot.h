#pragma once

namespace os32 {

class SdManager;

// Capture current LVGL screen and save as BMP to /sdcard/DCIM/SCR_xxxx.bmp
// Returns true on success. Does not work during direct render (camera).
bool screenshot_save(SdManager *sd);

} // namespace os32
