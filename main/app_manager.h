#pragma once

#include "app.h"
#include "menu.h"
#include "lvgl.h"

namespace os32 {

constexpr int MAX_APPS = 8;

class WifiManager;

class AppManager {
public:
    void init(lv_display_t *display, WifiManager *wifi);
    void register_app(App *app);
    void update(uint32_t dt_ms);
    void on_button(ButtonId btn, ButtonEvent event);
    void show_launcher();
    void get_status_text(char *line1, char *line2);
    bool active_uses_direct_render() const;
    void refresh_header();

private:
    void launch_app(int index);
    void return_to_launcher();
    void update_status_bar();

    lv_display_t *display_ = nullptr;
    WifiManager *wifi_ = nullptr;
    lv_obj_t *launcher_screen_ = nullptr;
    lv_obj_t *status_time_ = nullptr;
    lv_obj_t *status_wifi_ = nullptr;

    App *apps_[MAX_APPS] = {};
    int app_count_ = 0;

    int active_app_ = -1;  // -1 = launcher
    lv_obj_t *app_screen_ = nullptr;
    lv_obj_t *app_header_label_ = nullptr;

    Menu launcher_menu_;
};

} // namespace os32
