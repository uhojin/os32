#include "app_manager.h"
#include "os32.h"
#include "wifi_manager.h"
#include "timezone.h"
#include "esp_log.h"
#include <cstdio>
#include <ctime>

static const char *TAG = "app_mgr";

namespace os32 {

void AppManager::init(lv_display_t *display, WifiManager *wifi)
{
    display_ = display;
    wifi_ = wifi;
}

void AppManager::register_app(App *app)
{
    if (app_count_ >= MAX_APPS) {
        ESP_LOGE(TAG, "Max apps reached");
        return;
    }
    apps_[app_count_++] = app;
    ESP_LOGI(TAG, "Registered: %s", app->name());
}

void AppManager::show_launcher()
{
    int saved_cursor = 0;
    if (launcher_screen_) {
        saved_cursor = launcher_menu_.cursor();
        launcher_menu_.invalidate();  // Rows are children of screen; don't double-delete
        lv_obj_delete(launcher_screen_);
    }

    launcher_screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(launcher_screen_, color::bg(), 0);

    // Status bar — same style as app headers (bg1 background)
    lv_obj_t *bar = lv_obj_create(launcher_screen_);
    lv_obj_set_size(bar, LCD_H_RES, HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, color::bg1(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    int label_y = (HEADER_H - FONT_H) / 2;

    status_time_ = lv_label_create(bar);
    lv_obj_set_style_text_color(status_time_, color::fg(), 0);
    lv_obj_set_style_text_font(status_time_, font_mono(), 0);
    lv_obj_set_pos(status_time_, PAD, label_y);
    lv_label_set_text(status_time_, "");

    status_wifi_ = lv_label_create(bar);
    lv_obj_set_style_text_font(status_wifi_, &lv_font_montserrat_14, 0);
    lv_obj_align(status_wifi_, LV_ALIGN_TOP_RIGHT, -PAD, label_y);
    lv_label_set_text(status_wifi_, "");

    // Menu items via reusable Menu widget
    const char *labels[MAX_APPS];
    for (int i = 0; i < app_count_; i++) {
        labels[i] = apps_[i]->name();
    }
    launcher_menu_.create(launcher_screen_, CONTENT_Y, labels, app_count_);
    launcher_menu_.set_cursor(saved_cursor);

    active_app_ = -1;
    lv_screen_load(launcher_screen_);
}

void AppManager::launch_app(int index)
{
    if (index < 0 || index >= app_count_) return;

    app_screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(app_screen_, color::bg(), 0);
    lv_obj_set_style_pad_all(app_screen_, 0, 0);
    lv_obj_set_scrollbar_mode(app_screen_, LV_SCROLLBAR_MODE_OFF);

    // Header bar
    lv_obj_t *header = lv_obj_create(app_screen_);
    lv_obj_set_size(header, LCD_H_RES, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(header, color::bg1(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_left(header, PAD, 0);
    lv_obj_set_style_pad_top(header, (HEADER_H - FONT_H) / 2, 0);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);

    app_header_label_ = lv_label_create(header);
    lv_obj_set_style_text_font(app_header_label_, font_bold(), 0);
    lv_obj_set_style_text_color(app_header_label_, color::aqua(), 0);

    active_app_ = index;
    apps_[active_app_]->on_enter(app_screen_);
    char header_buf[48];
    apps_[active_app_]->get_header_text(header_buf, sizeof(header_buf));
    lv_label_set_text(app_header_label_, header_buf);
    lv_screen_load(app_screen_);

    ESP_LOGI(TAG, "Launched: %s", apps_[active_app_]->name());
}

void AppManager::return_to_launcher()
{
    if (active_app_ >= 0) {
        apps_[active_app_]->on_exit();
        ESP_LOGI(TAG, "Exited: %s", apps_[active_app_]->name());
    }

    app_header_label_ = nullptr;
    lv_obj_t *old_screen = app_screen_;
    app_screen_ = nullptr;

    show_launcher();

    if (old_screen) {
        lv_obj_delete(old_screen);
    }
}

void AppManager::update_status_bar()
{
    if (!status_time_ || !status_wifi_) return;

    // NTP time — show if ever synced
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm ti;
        localtime_r(&now, &ti);
        char buf[12];
        if (clock_is_24h()) {
            snprintf(buf, sizeof(buf), "%02d:%02d",
                     ti.tm_hour, ti.tm_min);
        } else {
            int h = ti.tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(buf, sizeof(buf), "%d:%02d %s",
                     h, ti.tm_min, ti.tm_hour >= 12 ? "PM" : "AM");
        }
        lv_label_set_text(status_time_, buf);
    }

    // WiFi status
    if (wifi_) {
        WifiState st = wifi_->state();
        if (st == WifiState::CONNECTED) {
            lv_obj_set_style_text_color(status_wifi_, color::green(), 0);
            lv_label_set_text(status_wifi_, LV_SYMBOL_WIFI);
        } else if (st == WifiState::CONNECTING) {
            lv_obj_set_style_text_color(status_wifi_, color::yellow(), 0);
            lv_label_set_text(status_wifi_, LV_SYMBOL_WIFI);
        } else if (st == WifiState::AP_ACTIVE) {
            lv_obj_set_style_text_color(status_wifi_, color::blue(), 0);
            lv_label_set_text(status_wifi_, LV_SYMBOL_WIFI);
        } else {
            lv_obj_set_style_text_color(status_wifi_, color::fg_dim(), 0);
            lv_label_set_text(status_wifi_, "");
        }
    }
}

void AppManager::update(uint32_t dt_ms)
{
    if (active_app_ >= 0) {
        apps_[active_app_]->on_update(dt_ms);
        if (app_header_label_) {
            char buf[48];
            apps_[active_app_]->get_header_text(buf, sizeof(buf));
            lv_label_set_text(app_header_label_, buf);
        }
    } else {
        update_status_bar();
    }
}

void AppManager::on_button(ButtonId btn, ButtonEvent event)
{
    // Pass RELEASE to active apps (needed for combo detection),
    // but ignore for launcher
    if (event == ButtonEvent::RELEASE) {
        if (active_app_ >= 0)
            apps_[active_app_]->on_button(btn, event);
        return;
    }

    if (active_app_ == -1) {
        switch (btn) {
            case ButtonId::UP:
                launcher_menu_.move_up();
                break;
            case ButtonId::DOWN:
                launcher_menu_.move_down();
                break;
            case ButtonId::RIGHT:
                if (event == ButtonEvent::PRESS) launch_app(launcher_menu_.cursor());
                break;
            default:
                break;
        }
    } else {
        bool consumed = apps_[active_app_]->on_button(btn, event);
        if (!consumed && btn == ButtonId::LEFT && event == ButtonEvent::PRESS) {
            return_to_launcher();
        }
    }
}

void AppManager::get_status_text(char *line1, char *line2)
{
    if (active_app_ >= 0) {
        apps_[active_app_]->get_status_text(line1, line2);
    } else {
        int cur = launcher_menu_.cursor();
        char pos[8];
        snprintf(pos, sizeof(pos), "%d/%d", (cur + 1) & 0xF, app_count_ & 0xF);
        snprintf(line1, 17, "%*s", 16, pos);
        snprintf(line2, 17, "> %-14s", apps_[cur]->name());
    }
}

void AppManager::refresh_header()
{
    if (active_app_ >= 0 && app_header_label_) {
        char buf[48];
        apps_[active_app_]->get_header_text(buf, sizeof(buf));
        lv_label_set_text(app_header_label_, buf);
    }
}

bool AppManager::active_uses_direct_render() const
{
    if (active_app_ >= 0) {
        return apps_[active_app_]->uses_direct_render();
    }
    return false;
}

} // namespace os32
