#include "backlight.h"
#include "os32.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace os32 {

static constexpr ledc_timer_t BL_TIMER   = LEDC_TIMER_1;
static constexpr ledc_channel_t BL_CHAN   = LEDC_CHANNEL_1;
static constexpr int BL_FREQ_HZ          = 5000;
static constexpr ledc_timer_bit_t BL_RES = LEDC_TIMER_8_BIT;  // 0-255

static constexpr const char *NVS_NS  = "display";
static constexpr const char *NVS_KEY = "brightness";

static uint8_t current_pct = 100;

void backlight_init()
{
    // Release any RTC hold from deep sleep (pin stays latched across reboot)
    gpio_hold_dis(PIN_BL);
    gpio_reset_pin(PIN_BL);

    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = BL_TIMER;
    timer.duty_resolution = BL_RES;
    timer.freq_hz = BL_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t chan = {};
    chan.speed_mode = LEDC_LOW_SPEED_MODE;
    chan.channel = BL_CHAN;
    chan.timer_sel = BL_TIMER;
    chan.gpio_num = PIN_BL;
    chan.duty = 255;  // Full brightness on init
    chan.hpoint = 0;
    ledc_channel_config(&chan);
}

void backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    current_pct = percent;
    uint32_t duty = (255 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHAN, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHAN);
}

uint8_t backlight_get()
{
    return current_pct;
}

void backlight_save()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY, current_pct);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void backlight_load()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val = 100;
        if (nvs_get_u8(nvs, NVS_KEY, &val) == ESP_OK && val >= 10 && val <= 100) {
            backlight_set(val);
        }
        nvs_close(nvs);
    }
}

void backlight_sleep()
{
    // Stop LEDC and force pin fully LOW, hold state through sleep
    ledc_stop(LEDC_LOW_SPEED_MODE, BL_CHAN, 0);
    gpio_reset_pin(PIN_BL);
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, 0);
    gpio_hold_en(PIN_BL);
}

void backlight_wake()
{
    gpio_hold_dis(PIN_BL);
    gpio_reset_pin(PIN_BL);
    backlight_init();
    backlight_set(current_pct);
}

} // namespace os32
