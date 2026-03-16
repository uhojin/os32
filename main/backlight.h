#pragma once

#include <cstdint>

namespace os32 {

void backlight_init();
void backlight_set(uint8_t percent);  // 0-100
uint8_t backlight_get();
void backlight_save();    // Persist current brightness to NVS
void backlight_load();    // Load saved brightness from NVS (call after init)
void backlight_sleep();   // Stop PWM, pin LOW for sleep
void backlight_wake();

} // namespace os32
