#pragma once

#include "lvgl.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace os32 {

enum class ButtonId : uint8_t {
    UP, DOWN, LEFT, RIGHT
};

enum class ButtonEvent : uint8_t {
    PRESS, RELEASE, REPEAT
};

class App {
public:
    virtual ~App() = default;

    virtual const char* name() const = 0;

    // App creates its LVGL widgets on the provided screen
    virtual void on_enter(lv_obj_t *screen) = 0;

    // Called each main loop iteration. dt_ms = time since last call.
    virtual void on_update(uint32_t dt_ms) = 0;

    // Called when leaving the app. Don't delete LVGL objects — screen cleanup is automatic.
    virtual void on_exit() = 0;

    // Return true if consumed. Unconsumed LEFT = back to launcher.
    virtual bool on_button(ButtonId btn, ButtonEvent event) = 0;

    // Text for 1602 LCD. line1/line2 are 17-byte buffers (16 chars + null).
    virtual void get_status_text(char *line1, char *line2) = 0;

    // Header bar title (e.g. "Settings > WiFi"). Default: app name. buf has len bytes.
    virtual void get_header_text(char *buf, std::size_t len) const {
        if (len) { std::strncpy(buf, name(), len - 1); buf[len - 1] = '\0'; }
    }

    // If true, app bypasses LVGL and writes directly to LCD panel.
    // Main loop skips lv_timer_handler while this app is active.
    virtual bool uses_direct_render() const { return false; }
};

} // namespace os32
