#include "os32.h"
#include "app_manager.h"
#include "lcd1602.h"
#include "wifi_manager.h"
#include "sd_manager.h"
#include "apps/app_sysmon.h"
#include "apps/app_settings.h"
#include "apps/app_camera.h"
#include "apps/app_files.h"
#include "apps/app_spotify.h"
#include "backlight.h"
#include "idle.h"
#include "screenshot.h"
#include "file_server.h"
#include "timezone.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"

static const char *TAG = "os32";

using namespace os32;

// LVGL tick
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

// LVGL display handle (for DMA done callback)
static lv_display_t *s_lvgl_display = nullptr;

// Called by esp_lcd when SPI DMA transfer completes
static bool lvgl_flush_done_cb(esp_lcd_panel_io_handle_t /*io*/,
                               esp_lcd_panel_io_event_data_t * /*data*/,
                               void *user_ctx)
{
    (void)user_ctx;
    lv_display_flush_ready(s_lvgl_display);
    return false;
}

// LVGL flush — starts async DMA, completion signaled by lvgl_flush_done_cb
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    auto panel = static_cast<esp_lcd_panel_handle_t>(lv_display_get_user_data(disp));
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;

    lv_draw_sw_rgb565_swap(px_map, (x2 - x1) * (y2 - y1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, px_map);
}

// Button state for edge detection + hold repeat
static constexpr uint32_t HOLD_INITIAL_MS = 500;
static constexpr uint32_t HOLD_REPEAT_MS  = 120;

struct ButtonState {
    gpio_num_t pin;
    ButtonId id;
    bool prev;
    uint32_t hold_ms;
    uint32_t repeat_ms;
};

static ButtonState buttons[] = {
    {BTN_UP,    ButtonId::UP,    false, 0, 0},
    {BTN_DOWN,  ButtonId::DOWN,  false, 0, 0},
    {BTN_LEFT,  ButtonId::LEFT,  false, 0, 0},
    {BTN_RIGHT, ButtonId::RIGHT, false, 0, 0},
};

static bool poll_buttons(AppManager &mgr, uint32_t dt_ms)
{
    bool any_activity = false;
    for (auto &b : buttons) {
        bool pressed = !gpio_get_level(b.pin);
        if (pressed && !b.prev) {
            any_activity = true;
            mgr.on_button(b.id, ButtonEvent::PRESS);
            b.hold_ms = 0;
            b.repeat_ms = 0;
        } else if (!pressed && b.prev) {
            any_activity = true;
            mgr.on_button(b.id, ButtonEvent::RELEASE);
            b.hold_ms = 0;
            b.repeat_ms = 0;
        } else if (pressed && b.id != ButtonId::LEFT) {
            b.hold_ms += dt_ms;
            if (b.hold_ms >= HOLD_INITIAL_MS) {
                b.repeat_ms += dt_ms;
                if (b.repeat_ms >= HOLD_REPEAT_MS) {
                    b.repeat_ms = 0;
                    mgr.on_button(b.id, ButtonEvent::REPEAT);
                }
            }
        }
        b.prev = pressed;
    }
    return any_activity;
}

static AppManager app_manager;
static LCD1602 lcd1602;
static WifiManager wifi_manager;
static SdManager sd_manager;
static IdleTimer idle_timer;
static FileServer file_server;

extern "C" void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    // Init SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus");
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = PIN_SCLK;
    bus_cfg.mosi_io_num = PIN_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Init panel IO
    ESP_LOGI(TAG, "Initializing panel IO");
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = PIN_DC;
    io_config.cs_gpio_num = PIN_CS;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lvgl_flush_done_cb;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

    // Init ST7789 panel
    ESP_LOGI(TAG, "Initializing ST7789 panel");
    esp_lcd_panel_handle_t panel_handle = nullptr;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Store panel handle for direct access (camera bypass)
    lcd_panel() = panel_handle;

    // Init backlight PWM (brightness loaded after NVS init)
    backlight_init();

    // Init LVGL
    ESP_LOGI(TAG, "Initializing LVGL");
    lv_init();

    auto *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    s_lvgl_display = display;
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    size_t buf_size = LCD_H_RES * 60 * sizeof(lv_color16_t);
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    lv_display_set_buffers(display, buf1, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lvgl_tick_cb;
    tick_args.name = "lvgl_tick";
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2 * 1000));

    // Boot screen
    lv_obj_t *boot_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(boot_screen, color::bg(), 0);

    lv_obj_t *boot_title = lv_label_create(boot_screen);
    lv_obj_set_style_text_font(boot_title, font_bold(), 0);
    lv_obj_set_style_text_color(boot_title, color::yellow(), 0);
    lv_label_set_text(boot_title, "os32");
    lv_obj_align(boot_title, LV_ALIGN_CENTER, 0, -16);

    lv_obj_t *boot_bar = lv_bar_create(boot_screen);
    lv_obj_set_size(boot_bar, 160, 4);
    lv_obj_align(boot_bar, LV_ALIGN_CENTER, 0, 8);
    lv_bar_set_range(boot_bar, 0, 100);
    lv_bar_set_value(boot_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(boot_bar, color::bg2(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(boot_bar, color::fg_dim(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(boot_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_screen_load(boot_screen);
    lv_timer_handler();

    auto boot_progress = [&](int pct) {
        lv_bar_set_value(boot_bar, pct, LV_ANIM_OFF);
        lv_timer_handler();
    };

    boot_progress(10);

    // Init buttons
    constexpr gpio_num_t btn_pins[] = {BTN_LEFT, BTN_DOWN, BTN_UP, BTN_RIGHT};
    for (auto pin : btn_pins) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << pin);
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }

    boot_progress(30);

    // Init SD card
    if (sd_manager.init()) {
        ESP_LOGI(TAG, "SD: %.1f MB total, %.1f MB free",
                 sd_manager.total_bytes() / 1048576.0,
                 sd_manager.free_bytes() / 1048576.0);
    }

    boot_progress(50);

    // Init WiFi manager (loads saved credentials, doesn't connect yet)
    wifi_manager.init();

    // Init mDNS — advertise as os32.local on the network
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("os32");
    mdns_instance_name_set("os32");
    mdns_service_add("os32", "_http", "_tcp", 8080, NULL, 0);

    boot_progress(70);

    // Load saved settings (must be after NVS init in wifi_manager)
    backlight_load();
    idle_timer.init();
    timezone_init();

    // Init 1602 I2C LCD
    if (lcd1602.init(LCD1602_SDA, LCD1602_SCL, LCD1602_ADDR)) {
        lcd1602.set_line(0, "os32");
        lcd1602.set_line(1, "Starting...");
    }

    boot_progress(90);

    // Init app manager and register apps
    app_manager.init(display, &wifi_manager);

    static SpotifyApp spotify_app(&wifi_manager);
    app_manager.register_app(&spotify_app);

    static CameraApp camera_app(&sd_manager);
    app_manager.register_app(&camera_app);

    static FilesApp files_app(&sd_manager, &wifi_manager, &file_server);
    app_manager.register_app(&files_app);

    static SysMonApp sysmon_app;
    app_manager.register_app(&sysmon_app);

    static SettingsApp settings_app(&wifi_manager, &idle_timer, &app_manager);
    app_manager.register_app(&settings_app);

    boot_progress(100);

    app_manager.show_launcher();
    lv_obj_delete(boot_screen);

    // Auto-connect if credentials exist
    if (wifi_manager.has_saved_credentials()) {
        wifi_manager.connect_saved();
    }

    ESP_LOGI(TAG, "os32 ready");

    // Main loop
    uint32_t last_tick = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t status_accum = 500; // trigger immediate first update
    uint32_t status_hold = 0;    // when >0, suppress status updates (countdown)
    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t dt = now - last_tick;
        last_tick = now;

        // When display is sleeping, poll buttons but don't forward to apps
        if (idle_timer.state() == IdleState::SLEEP) {
            bool any_pressed = false;
            for (auto &b : buttons) {
                bool pressed = !gpio_get_level(b.pin);
                if (pressed && !b.prev) any_pressed = true;
                b.prev = pressed;
            }
            if (any_pressed) {
                // Wake displays, swallow this button press
                esp_lcd_panel_disp_on_off(lcd_panel(), true);
                lcd1602.set_backlight(true);
                backlight_load();
                lv_obj_invalidate(lv_screen_active());
                idle_timer.reset();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Screenshot combo: UP+DOWN pressed simultaneously (check before button dispatch)
        bool screenshot_combo = false;
        {
            static bool combo_active = false;
            bool up_held = !gpio_get_level(BTN_UP);
            bool down_held = !gpio_get_level(BTN_DOWN);
            if (up_held && down_held) {
                screenshot_combo = true;
                if (!combo_active && !app_manager.active_uses_direct_render()) {
                    combo_active = true;
                    if (screenshot_save(&sd_manager)) {
                        lcd1602.set_line(0, "Screenshot saved");
                        lcd1602.set_line(1, "");
                        status_hold = 2000;  // hold message for 2s
                    }
                }
            } else {
                combo_active = false;
            }
        }

        // Only dispatch buttons when screenshot combo is not active
        if (!screenshot_combo) {
            bool input = poll_buttons(app_manager, dt);
            if (input) idle_timer.reset();
        }

        // Don't idle during direct render (e.g. camera preview)
        if (app_manager.active_uses_direct_render()) idle_timer.reset();

        idle_timer.update(dt);

        if (idle_timer.state() == IdleState::SLEEP) {
            // Just entered sleep — turn everything off
            backlight_set(0);
            esp_lcd_panel_disp_on_off(lcd_panel(), false);
            lcd1602.set_backlight(false);
            continue;
        }

        app_manager.update(dt);

        // Stop file server if WiFi drops while server is running
        if (file_server.running() && wifi_manager.state() != WifiState::CONNECTED) {
            file_server.stop();
        }

        // Update 1602 status display every 500ms
        if (status_hold > 0) {
            status_hold = (dt < status_hold) ? status_hold - dt : 0;
            status_accum = 0;
        } else {
            status_accum += dt;
            if (status_accum >= 500) {
                status_accum = 0;
                char line1[17], line2[17];
                app_manager.get_status_text(line1, line2);
                lcd1602.set_line(0, line1);
                lcd1602.set_line(1, line2);
            }
        }

        if (app_manager.active_uses_direct_render()) {
            // Camera/direct-render mode: skip LVGL, minimal delay for button polling
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            uint32_t wait = lv_timer_handler();
            if (wait > 500) wait = 500;
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
    }
}
