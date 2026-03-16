#pragma once

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace os32 {

// ST7789 SPI pins
constexpr gpio_num_t PIN_MOSI = GPIO_NUM_47;
constexpr gpio_num_t PIN_SCLK = GPIO_NUM_46;
constexpr gpio_num_t PIN_CS   = GPIO_NUM_41;
constexpr gpio_num_t PIN_DC   = GPIO_NUM_14;
constexpr gpio_num_t PIN_RST  = GPIO_NUM_21;
constexpr gpio_num_t PIN_BL   = GPIO_NUM_19;

// Button pins (active LOW, internal pull-up)
constexpr gpio_num_t BTN_LEFT  = GPIO_NUM_3;  // Back
constexpr gpio_num_t BTN_DOWN  = GPIO_NUM_0;
constexpr gpio_num_t BTN_UP    = GPIO_NUM_2;
constexpr gpio_num_t BTN_RIGHT = GPIO_NUM_1;  // Select/Enter

// SD card (SDMMC 1-bit, onboard slot)
constexpr gpio_num_t SD_CMD = GPIO_NUM_38;
constexpr gpio_num_t SD_CLK = GPIO_NUM_39;
constexpr gpio_num_t SD_D0  = GPIO_NUM_40;
constexpr const char* SD_MOUNT = "/sdcard";

// 1602 I2C LCD
constexpr gpio_num_t LCD1602_SDA = GPIO_NUM_45;
constexpr gpio_num_t LCD1602_SCL = GPIO_NUM_42;
constexpr uint8_t LCD1602_ADDR  = 0x27;  // PCF8574 default (try 0x3F if not found)

// Display dimensions
constexpr int LCD_H_RES = 320;
constexpr int LCD_V_RES = 240;

// Gruvbox color palette
namespace color {
    inline lv_color_t bg()      { return lv_color_make(0x28, 0x28, 0x28); } // #282828
    inline lv_color_t bg1()     { return lv_color_make(0x3c, 0x38, 0x36); } // #3c3836
    inline lv_color_t bg2()     { return lv_color_make(0x50, 0x49, 0x45); } // #504945
    inline lv_color_t fg()      { return lv_color_make(0xeb, 0xdb, 0xb2); } // #ebdbb2
    inline lv_color_t fg_dim()  { return lv_color_make(0x92, 0x83, 0x74); } // #928374 gray
    inline lv_color_t red()     { return lv_color_make(0xfb, 0x49, 0x34); } // #fb4934
    inline lv_color_t green()   { return lv_color_make(0xb8, 0xbb, 0x26); } // #b8bb26
    inline lv_color_t yellow()  { return lv_color_make(0xfa, 0xbd, 0x2f); } // #fabd2f
    inline lv_color_t blue()    { return lv_color_make(0x83, 0xa5, 0x98); } // #83a598
    inline lv_color_t purple()  { return lv_color_make(0xd3, 0x86, 0x9b); } // #d3869b
    inline lv_color_t aqua()    { return lv_color_make(0x8e, 0xc0, 0x7c); } // #8ec07c
    inline lv_color_t orange()  { return lv_color_make(0xfe, 0x80, 0x19); } // #fe8019
}

} // namespace os32

// Font declarations (C linkage — defined in generated .c files)
extern "C" {
extern const lv_font_t font_gohu11;
extern const lv_font_t font_gohu14;
extern const lv_font_t font_gohu14b;
}

namespace os32 {

inline const lv_font_t* font_mono()    { return &font_gohu14; }
inline const lv_font_t* font_bold()    { return &font_gohu14b; }
inline const lv_font_t* font_small()   { return &font_gohu11; }

// Layout constants derived from font metrics
constexpr int FONT_H = 14;   // font_mono() line height in px
constexpr int FONT_W = 8;    // font_mono() character advance in px
constexpr int PAD = 8;       // standard horizontal padding

// App header bar — font height + vertical padding
constexpr int HEADER_H = FONT_H + 10;
// Content area starts below header
constexpr int CONTENT_Y = HEADER_H + PAD;
// Max chars per line with standard padding
constexpr int MAX_CHARS = (LCD_H_RES - PAD) / FONT_W;

// Direct LCD panel access (set by main.cpp, used by camera for fast DMA writes)
inline esp_lcd_panel_handle_t& lcd_panel() {
    static esp_lcd_panel_handle_t handle = nullptr;
    return handle;
}

} // namespace os32
