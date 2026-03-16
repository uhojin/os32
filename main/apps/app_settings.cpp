#include "app_settings.h"
#include "app_manager.h"
#include "os32.h"
#include "backlight.h"
#include "idle.h"
#include "wifi_manager.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <ctime>

#include "timezone.h"

static const char *TAG = "settings";

namespace os32 {

static const char *POWER_ACTIONS[] = { "Reboot", "Sleep", "Shutdown" };
static constexpr int POWER_ACTION_COUNT = 3;

void SettingsApp::on_enter(lv_obj_t *screen)
{
    screen_ = screen;
    page_ = Page::MAIN;
    main_cursor_ = 0;
    display_cursor_ = 0;
    power_action_ = -1;
    refresh_accum_ = 0;
    last_wifi_state_ = wifi_->state();
    build_page();
}

void SettingsApp::build_page()
{
    // Clean up previous UI elements
    menu_.destroy();
    if (info_label_) {
        lv_obj_delete(info_label_);
        info_label_ = nullptr;
    }
    if (brightness_bar_) {
        lv_obj_delete(brightness_bar_);
        brightness_bar_ = nullptr;
        brightness_label_ = nullptr;
    }
    if (brightness_hint_) {
        lv_obj_delete(brightness_hint_);
        brightness_hint_ = nullptr;
    }

    int menu_y = CONTENT_Y;

    switch (page_) {
    case Page::MAIN: {
        const char *items[] = { "WiFi", "Display", "Date & Time", "Power" };
        menu_.create(screen_, menu_y, items, 4);
        menu_.set_cursor(main_cursor_);
        break;
    }

    case Page::WIFI: {
        // Info header (non-selectable status lines)
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        char info[128];
        char ssid[33], ip[16];
        wifi_->get_ssid(ssid, sizeof(ssid));
        wifi_->get_ip(ip, sizeof(ip));
        auto state = wifi_->state();

        const char *status;
        switch (state) {
        case WifiState::CONNECTED:  status = "Connected"; break;
        case WifiState::CONNECTING: status = "Connecting..."; break;
        case WifiState::AP_ACTIVE:  status = "AP Active"; break;
        case WifiState::FAILED:     status = "Failed"; break;
        default:                    status = "Not configured"; break;
        }

        if (state == WifiState::CONNECTED) {
            snprintf(info, sizeof(info), "WiFi: %s\nSSID: %s\nIP: %s", status, ssid, ip);
        } else {
            snprintf(info, sizeof(info), "WiFi: %s", status);
        }
        lv_label_set_text(info_label_, info);

        // Count info lines to offset menu below
        int info_lines = 1;
        for (const char *p = info; *p; p++) {
            if (*p == '\n') info_lines++;
        }
        menu_y = CONTENT_Y + info_lines * FONT_H + PAD;

        if (state == WifiState::CONNECTED) {
            const char *items[] = { "Back", "Change Network", "Forget Network" };
            menu_.create(screen_, menu_y, items, 3);
        } else {
            const char *items[] = { "Setup WiFi" };
            menu_.create(screen_, menu_y, items, 1);
        }
        break;
    }

    case Page::WIFI_CONFIRM_FORGET: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
        lv_label_set_text(info_label_,
            "Forget saved network?\n"
            "\n"
            "[LEFT] Cancel\n"
            "[RIGHT] Confirm");
        break;
    }

    case Page::WIFI_SETUP:
        // Handled by show_wifi_setup()
        break;

    case Page::DISPLAY: {
        const char *items[] = { "Brightness", "Sleep Timer" };
        menu_.create(screen_, menu_y, items, 2);
        menu_.set_cursor(display_cursor_);
        break;
    }

    case Page::BRIGHTNESS: {
        int bar_w = LCD_H_RES - 4 * PAD;
        uint8_t pct = backlight_get();

        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        // Bar background
        brightness_bar_ = lv_obj_create(screen_);
        lv_obj_set_size(brightness_bar_, bar_w, FONT_H + 4);
        lv_obj_set_pos(brightness_bar_, 2 * PAD, CONTENT_Y + FONT_H * 2 + PAD);
        lv_obj_set_style_bg_color(brightness_bar_, color::bg2(), 0);
        lv_obj_set_style_bg_opa(brightness_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(brightness_bar_, 0, 0);
        lv_obj_set_style_border_width(brightness_bar_, 0, 0);
        lv_obj_set_style_pad_all(brightness_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(brightness_bar_, LV_SCROLLBAR_MODE_OFF);

        // Bar fill (child of bar background)
        int fill_w = (bar_w * pct) / 100;
        if (fill_w < 1 && pct > 0) fill_w = 1;
        brightness_label_ = lv_obj_create(brightness_bar_);
        lv_obj_set_size(brightness_label_, fill_w, FONT_H + 4);
        lv_obj_set_style_bg_color(brightness_label_, color::yellow(), 0);
        lv_obj_set_style_bg_opa(brightness_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(brightness_label_, 0, 0);
        lv_obj_set_style_border_width(brightness_label_, 0, 0);
        lv_obj_set_style_pad_all(brightness_label_, 0, 0);
        lv_obj_set_pos(brightness_label_, 0, 0);
        lv_obj_set_scrollbar_mode(brightness_label_, LV_SCROLLBAR_MODE_OFF);

        // Controls hint below bar
        brightness_hint_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(brightness_hint_, font_mono(), 0);
        lv_obj_set_style_text_color(brightness_hint_, color::fg_dim(), 0);
        lv_obj_align(brightness_hint_, LV_ALIGN_TOP_MID, 0, CONTENT_Y + FONT_H * 2 + PAD + (FONT_H + 4) + PAD);
        lv_label_set_text(brightness_hint_, "[UP/DOWN] Adjust");

        update_brightness_display();
        break;
    }

    case Page::SLEEP_TIMER: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        char buf[48];
        snprintf(buf, sizeof(buf), "Sleep: %s",
                 IdleTimer::timeout_label(idle_->timeout_sec()));
        lv_label_set_text(info_label_, buf);

        brightness_hint_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(brightness_hint_, font_mono(), 0);
        lv_obj_set_style_text_color(brightness_hint_, color::fg_dim(), 0);
        lv_obj_align(brightness_hint_, LV_ALIGN_TOP_MID, 0, CONTENT_Y + FONT_H * 2 + PAD);
        lv_label_set_text(brightness_hint_, "[UP/DOWN] Adjust");
        break;
    }

    case Page::DATE_TIME: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        brightness_hint_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(brightness_hint_, font_mono(), 0);
        lv_obj_set_style_text_color(brightness_hint_, color::fg_dim(), 0);
        lv_obj_set_pos(brightness_hint_, PAD, CONTENT_Y + FONT_H * 8 + PAD);
        lv_label_set_text(brightness_hint_,
            "[UP/DOWN] Timezone\n"
            "[RIGHT]   12/24h");

        update_datetime_display();
        break;
    }

    case Page::POWER: {
        int saved_cursor = (power_action_ >= 0) ? power_action_ : 0;
        menu_.create(screen_, menu_y, POWER_ACTIONS, POWER_ACTION_COUNT);
        menu_.set_cursor(saved_cursor);
        break;
    }

    case Page::POWER_CONFIRM: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        char buf[64];
        snprintf(buf, sizeof(buf),
            "%s?\n"
            "\n"
            "[LEFT] Cancel\n"
            "[RIGHT] Confirm",
            POWER_ACTIONS[power_action_]);
        lv_label_set_text(info_label_, buf);
        break;
    }
    }
}

void SettingsApp::show_wifi_setup()
{
    page_ = Page::WIFI_SETUP;
    menu_.destroy();
    if (info_label_) {
        lv_obj_delete(info_label_);
        info_label_ = nullptr;
    }

    wifi_->start_ap();

    char ap_ssid[17], ap_pass[9];
    wifi_->get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    wifi_->get_ap_password(ap_pass, sizeof(ap_pass));

    info_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(info_label_, font_mono(), 0);
    lv_obj_set_style_text_color(info_label_, color::fg(), 0);
    lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "Connect your phone:\n"
        "\n"
        "Network:\n"
        "  %s\n"
        "Password:\n"
        "  %s\n"
        "\n"
        "A setup page will\n"
        "open automatically.\n"
        "\n"
        "[LEFT] Cancel",
        ap_ssid, ap_pass);

    lv_label_set_text(info_label_, buf);
}

void SettingsApp::on_update(uint32_t dt_ms)
{
    refresh_accum_ += dt_ms;
    if (refresh_accum_ < 1000) return;
    refresh_accum_ = 0;

    auto cur_state = wifi_->state();

    if (page_ == Page::WIFI_SETUP) {
        if (cur_state == WifiState::CONNECTED) {
            page_ = Page::WIFI;
            last_wifi_state_ = cur_state;
            build_page();
        }
    } else if (page_ == Page::WIFI) {
        if (cur_state != last_wifi_state_) {
            last_wifi_state_ = cur_state;
            build_page();
        } else {
            update_wifi_info_text();
        }
    } else if (page_ == Page::DATE_TIME) {
        update_datetime_display();
    }
}

void SettingsApp::update_wifi_info_text()
{
    if (page_ != Page::WIFI || !info_label_) return;

    char info[128];
    char ssid[33], ip[16];
    wifi_->get_ssid(ssid, sizeof(ssid));
    wifi_->get_ip(ip, sizeof(ip));
    auto state = wifi_->state();

    const char *status;
    switch (state) {
    case WifiState::CONNECTED:  status = "Connected"; break;
    case WifiState::CONNECTING: status = "Connecting..."; break;
    case WifiState::AP_ACTIVE:  status = "AP Active"; break;
    case WifiState::FAILED:     status = "Failed"; break;
    default:                    status = "Not configured"; break;
    }

    if (state == WifiState::CONNECTED) {
        snprintf(info, sizeof(info), "WiFi: %s\nSSID: %s\nIP: %s", status, ssid, ip);
    } else {
        snprintf(info, sizeof(info), "WiFi: %s", status);
    }
    lv_label_set_text(info_label_, info);
}

void SettingsApp::update_brightness_display()
{
    if (!info_label_ || !brightness_label_) return;
    uint8_t pct = backlight_get();

    char buf[32];
    snprintf(buf, sizeof(buf), "Brightness: %d%%", pct);
    lv_label_set_text(info_label_, buf);

    int bar_w = LCD_H_RES - 4 * PAD;
    int fill_w = (bar_w * pct) / 100;
    if (fill_w < 1 && pct > 0) fill_w = 1;
    lv_obj_set_size(brightness_label_, fill_w, FONT_H + 4);
}

void SettingsApp::update_datetime_display()
{
    if (!info_label_) return;

    time_t now = time(nullptr);
    bool synced = now > 1700000000;
    int tz_idx = timezone_get();
    const char *tz_name = TIMEZONES[tz_idx].label;

    bool h24 = clock_is_24h();
    const char *fmt_label = h24 ? "24h" : "12h";

    char buf[128];
    if (synced) {
        struct tm t;
        localtime_r(&now, &t);
        char time_str[16], date_str[16];
        if (h24) {
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     t.tm_hour, t.tm_min, t.tm_sec);
        } else {
            int h = t.tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(time_str, sizeof(time_str), "%d:%02d:%02d %s",
                     h, t.tm_min, t.tm_sec, t.tm_hour >= 12 ? "PM" : "AM");
        }
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", &t);
        snprintf(buf, sizeof(buf),
            "Time: %s\n"
            "Date: %s\n"
            "Zone: %s\n"
            "Mode: %s\n"
            "\n"
            "NTP:  Synced",
            time_str, date_str, tz_name, fmt_label);
    } else {
        snprintf(buf, sizeof(buf),
            "Time: --:--:--\n"
            "Date: ----.--.--\n"
            "Zone: %s\n"
            "Mode: %s\n"
            "\n"
            "NTP:  Not synced",
            tz_name, fmt_label);
    }
    lv_label_set_text(info_label_, buf);
}

// ---------------------------------------------------------------------------
// Power actions (moved from PowerApp)
// ---------------------------------------------------------------------------

void SettingsApp::wait_buttons_released()
{
    constexpr gpio_num_t pins[] = {BTN_LEFT, BTN_DOWN, BTN_UP, BTN_RIGHT};
    while (true) {
        bool any_pressed = false;
        for (auto pin : pins) {
            if (!gpio_get_level(pin)) { any_pressed = true; break; }
        }
        if (!any_pressed) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}

void SettingsApp::execute_power_action()
{
    switch (power_action_) {
    case 0: // Reboot
        ESP_LOGI(TAG, "Rebooting...");
        esp_restart();
        break;

    case 1: { // Sleep (light sleep — resume on wake)
        ESP_LOGI(TAG, "Entering light sleep...");
        wait_buttons_released();

        constexpr gpio_num_t wake_pins[] = {BTN_LEFT, BTN_DOWN, BTN_UP, BTN_RIGHT};
        for (auto pin : wake_pins) {
            gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
        }
        esp_sleep_enable_gpio_wakeup();

        // Update UI to power page and flush before sleep
        page_ = Page::POWER;
        build_page();
        mgr_->refresh_header();
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);

        backlight_sleep();
        esp_lcd_panel_disp_on_off(lcd_panel(), false);
        esp_light_sleep_start();

        // Reinit display (SPI peripheral state lost during sleep)
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_lcd_panel_init(lcd_panel());
        esp_lcd_panel_invert_color(lcd_panel(), true);
        esp_lcd_panel_swap_xy(lcd_panel(), true);
        esp_lcd_panel_mirror(lcd_panel(), false, true);
        esp_lcd_panel_disp_on_off(lcd_panel(), true);
        backlight_wake();

        lv_obj_invalidate(lv_screen_active());

        // Wait for WiFi disconnect event to process, then reconnect
        vTaskDelay(pdMS_TO_TICKS(500));
        if (wifi_ && wifi_->has_saved_credentials() &&
            wifi_->state() != WifiState::CONNECTED &&
            wifi_->state() != WifiState::CONNECTING) {
            wifi_->connect_saved();
        }

        ESP_LOGI(TAG, "Woke from light sleep");
        break;
    }

    case 2: { // Shutdown (deep sleep — full restart on wake)
        ESP_LOGI(TAG, "Entering deep sleep...");
        wait_buttons_released();

        // Clear display RAM so boot doesn't flash stale content
        lv_obj_t *blank = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(blank, color::bg(), 0);
        lv_screen_load(blank);
        lv_refr_now(nullptr);

        backlight_sleep();
        esp_lcd_panel_disp_on_off(lcd_panel(), false);

        // Enable RTC pull-ups — normal GPIO pull-ups are disabled during deep sleep
        constexpr gpio_num_t btn_pins[] = {BTN_LEFT, BTN_DOWN, BTN_UP, BTN_RIGHT};
        for (auto pin : btn_pins) {
            rtc_gpio_pullup_en(pin);
            rtc_gpio_pulldown_dis(pin);
        }

        uint64_t wake_mask = (1ULL << BTN_LEFT) | (1ULL << BTN_DOWN)
                           | (1ULL << BTN_UP) | (1ULL << BTN_RIGHT);
        esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);

        esp_deep_sleep_start();
        break;
    }
    }
}

void SettingsApp::on_exit()
{
    if (page_ == Page::WIFI_SETUP) {
        wifi_->stop_ap();
    }
    menu_.invalidate();
    info_label_ = nullptr;
    brightness_bar_ = nullptr;
    brightness_label_ = nullptr;
    brightness_hint_ = nullptr;
    screen_ = nullptr;
}

bool SettingsApp::on_button(ButtonId btn, ButtonEvent event)
{
    if (event == ButtonEvent::RELEASE) return false;

    if (page_ == Page::WIFI_SETUP) {
        if (btn == ButtonId::LEFT) {
            wifi_->stop_ap();
            page_ = Page::WIFI;
            build_page();
        }
        return true;
    }

    if (page_ == Page::WIFI_CONFIRM_FORGET) {
        if (btn == ButtonId::RIGHT) {
            wifi_->forget_credentials();
            wifi_->disconnect();
            page_ = Page::WIFI;
            build_page();
        } else if (btn == ButtonId::LEFT) {
            page_ = Page::WIFI;
            build_page();
        }
        return true;
    }

    if (page_ == Page::POWER_CONFIRM) {
        if (btn == ButtonId::RIGHT && event == ButtonEvent::PRESS) {
            execute_power_action();
        } else if (btn == ButtonId::LEFT && event == ButtonEvent::PRESS) {
            page_ = Page::POWER;
            build_page();
        }
        return true;
    }

    if (page_ == Page::SLEEP_TIMER) {
        if (btn == ButtonId::UP || btn == ButtonId::DOWN) {
            uint16_t cur = idle_->timeout_sec();
            int idx = 0;
            for (int i = 0; i < IdleTimer::TIMEOUT_COUNT; i++) {
                if (IdleTimer::TIMEOUT_OPTIONS[i] == cur) { idx = i; break; }
            }
            if (btn == ButtonId::UP && idx < IdleTimer::TIMEOUT_COUNT - 1) idx++;
            else if (btn == ButtonId::DOWN && idx > 0) idx--;
            idle_->set_timeout(IdleTimer::TIMEOUT_OPTIONS[idx]);

            char buf[48];
            snprintf(buf, sizeof(buf), "Sleep: %s",
                     IdleTimer::timeout_label(idle_->timeout_sec()));
            lv_label_set_text(info_label_, buf);
            return true;
        }
        if (btn == ButtonId::LEFT) {
            page_ = Page::DISPLAY;
            build_page();
            return true;
        }
        return true;
    }

    if (page_ == Page::BRIGHTNESS) {
        if (btn == ButtonId::UP || btn == ButtonId::DOWN) {
            uint8_t pct = backlight_get();
            if (btn == ButtonId::UP && pct < 100) pct += 10;
            else if (btn == ButtonId::DOWN && pct > 10) pct -= 10;
            backlight_set(pct);
            backlight_save();
            update_brightness_display();
            return true;
        }
        if (btn == ButtonId::LEFT) {
            page_ = Page::DISPLAY;
            build_page();
            return true;
        }
        return true;
    }

    if (page_ == Page::DATE_TIME) {
        if (btn == ButtonId::UP || btn == ButtonId::DOWN) {
            int idx = timezone_get();
            if (btn == ButtonId::UP) {
                idx = (idx + 1) % TIMEZONE_COUNT;
            } else {
                idx = (idx - 1 + TIMEZONE_COUNT) % TIMEZONE_COUNT;
            }
            timezone_set(idx);
            update_datetime_display();
            return true;
        }
        if (btn == ButtonId::RIGHT) {
            clock_set_24h(!clock_is_24h());
            update_datetime_display();
            return true;
        }
        if (btn == ButtonId::LEFT) {
            page_ = Page::MAIN;
            build_page();
            return true;
        }
        return true;
    }

    if (page_ == Page::POWER) {
        switch (btn) {
        case ButtonId::UP:
            menu_.move_up();
            return true;
        case ButtonId::DOWN:
            menu_.move_down();
            return true;
        case ButtonId::RIGHT:
            if (event == ButtonEvent::PRESS) {
                power_action_ = menu_.cursor();
                page_ = Page::POWER_CONFIRM;
                build_page();
            }
            return true;
        case ButtonId::LEFT:
            page_ = Page::MAIN;
            build_page();
            return true;
        default:
            return false;
        }
    }

    switch (btn) {
    case ButtonId::UP:
        menu_.move_up();
        return true;
    case ButtonId::DOWN:
        menu_.move_down();
        return true;
    case ButtonId::LEFT:
        if (page_ == Page::WIFI) {
            page_ = Page::MAIN;
            build_page();
            return true;
        }
        if (page_ == Page::DISPLAY) {
            page_ = Page::MAIN;
            build_page();
            return true;
        }
        return false;
    case ButtonId::RIGHT:
        if (page_ == Page::MAIN) {
            main_cursor_ = menu_.cursor();
            switch (main_cursor_) {
            case 0:  // WiFi
                page_ = Page::WIFI;
                build_page();
                break;
            case 1:  // Display
                page_ = Page::DISPLAY;
                build_page();
                break;
            case 2:  // Date & Time
                page_ = Page::DATE_TIME;
                build_page();
                break;
            case 3:  // Power
                page_ = Page::POWER;
                build_page();
                break;
            }
        } else if (page_ == Page::WIFI) {
            if (wifi_->state() == WifiState::CONNECTED) {
                switch (menu_.cursor()) {
                case 0: // Back
                        page_ = Page::MAIN;
                        build_page();
                        break;
                case 1: show_wifi_setup(); break;  // Reconfigure
                case 2: // Forget — confirm first
                        page_ = Page::WIFI_CONFIRM_FORGET;
                        build_page();
                        break;
                }
            } else {
                show_wifi_setup();
            }
        } else if (page_ == Page::DISPLAY) {
            display_cursor_ = menu_.cursor();
            switch (display_cursor_) {
            case 0:  // Brightness
                page_ = Page::BRIGHTNESS;
                build_page();
                break;
            case 1:  // Sleep Timer
                page_ = Page::SLEEP_TIMER;
                build_page();
                break;
            }
        }
        return true;
    default:
        return false;
    }
}

void SettingsApp::get_header_text(char *buf, std::size_t len) const
{
    if (len == 0) return;
    const char *title = "Settings";
    switch (page_) {
    case Page::MAIN: title = "Settings"; break;
    case Page::WIFI: title = "Settings > WiFi"; break;
    case Page::WIFI_SETUP: title = "Settings > WiFi > Setup"; break;
    case Page::WIFI_CONFIRM_FORGET: title = "Settings > WiFi > Forget"; break;
    case Page::DISPLAY: title = "Settings > Display"; break;
    case Page::BRIGHTNESS: title = "Settings > Brightness"; break;
    case Page::SLEEP_TIMER: title = "Settings > Sleep Timer"; break;
    case Page::DATE_TIME: title = "Settings > Date & Time"; break;
    case Page::POWER: title = "Settings > Power"; break;
    case Page::POWER_CONFIRM:
        if (power_action_ >= 0) {
            static char hdr[48];
            snprintf(hdr, sizeof(hdr), "Settings > Power > %s",
                     POWER_ACTIONS[power_action_]);
            title = hdr;
        } else {
            title = "Settings > Power";
        }
        break;
    }
    std::strncpy(buf, title, len - 1);
    buf[len - 1] = '\0';
}

void SettingsApp::get_status_text(char *line1, char *line2)
{
    if (page_ == Page::WIFI_SETUP) {
        char ap_pass[9];
        wifi_->get_ap_password(ap_pass, sizeof(ap_pass));
        snprintf(line1, 17, "AP: os32-setup");
        snprintf(line2, 17, "PW: %s", ap_pass);
    } else if (page_ == Page::POWER || page_ == Page::POWER_CONFIRM) {
        snprintf(line1, 17, "%-16s", "Power");
        if (page_ == Page::POWER_CONFIRM && power_action_ >= 0) {
            snprintf(line2, 17, "> %-14s", POWER_ACTIONS[power_action_]);
        } else {
            snprintf(line2, 17, "> %-14s", POWER_ACTIONS[menu_.cursor()]);
        }
    } else {
        auto state = wifi_->state();
        snprintf(line1, 17, "%-16s", "Settings");
        switch (state) {
        case WifiState::CONNECTED:  snprintf(line2, 17, "WiFi: OK"); break;
        case WifiState::CONNECTING: snprintf(line2, 17, "WiFi: ..."); break;
        default:                    snprintf(line2, 17, "WiFi: --"); break;
        }
    }
}

} // namespace os32
