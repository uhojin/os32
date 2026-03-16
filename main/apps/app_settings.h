#pragma once

#include "app.h"
#include "menu.h"
#include "wifi_manager.h"
#include "idle.h"

namespace os32 {

class AppManager;

class SettingsApp : public App {
public:
    SettingsApp(WifiManager *wifi, IdleTimer *idle, AppManager *mgr)
        : wifi_(wifi), idle_(idle), mgr_(mgr) {}
    const char* name() const override { return "Settings"; }
    void on_enter(lv_obj_t *screen) override;
    void on_update(uint32_t dt_ms) override;
    void on_exit() override;
    bool on_button(ButtonId btn, ButtonEvent event) override;
    void get_status_text(char *line1, char *line2) override;
    void get_header_text(char *buf, std::size_t len) const override;

private:
    enum class Page {
        MAIN, WIFI, WIFI_SETUP, WIFI_CONFIRM_FORGET,
        DISPLAY, BRIGHTNESS, SLEEP_TIMER,
        DATE_TIME,
        POWER, POWER_CONFIRM
    };
    void build_page();
    void show_wifi_setup();
    void update_wifi_info_text();
    void update_brightness_display();
    void update_datetime_display();
    void execute_power_action();
    void wait_buttons_released();

    WifiManager *wifi_;
    IdleTimer *idle_;
    AppManager *mgr_;
    Page page_ = Page::MAIN;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *info_label_ = nullptr;
    Menu menu_;
    lv_obj_t *brightness_bar_ = nullptr;
    lv_obj_t *brightness_label_ = nullptr;
    lv_obj_t *brightness_hint_ = nullptr;
    uint32_t refresh_accum_ = 0;
    WifiState last_wifi_state_ = WifiState::IDLE;
    int main_cursor_ = 0;
    int display_cursor_ = 0;
    int power_action_ = -1;
};

} // namespace os32
