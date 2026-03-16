#include "idle.h"
#include "backlight.h"
#include "nvs.h"

namespace os32 {

static constexpr const char *NVS_NS  = "display";
static constexpr const char *NVS_KEY = "sleep_tmr";

// Dim at 75% of timeout, sleep at 100%
static constexpr float DIM_FRACTION = 0.75f;
static constexpr uint8_t DIM_BRIGHTNESS = 10;

void IdleTimer::init()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t val = 0;
        if (nvs_get_u16(nvs, NVS_KEY, &val) == ESP_OK) {
            timeout_sec_ = val;
        }
        nvs_close(nvs);
    }
}

void IdleTimer::reset()
{
    idle_ms_ = 0;
    if (state_ == IdleState::DIM) {
        // Restore saved brightness
        backlight_load();
    }
    state_ = IdleState::ACTIVE;
}

void IdleTimer::update(uint32_t dt_ms)
{
    if (timeout_sec_ == 0 || state_ == IdleState::SLEEP) return;

    idle_ms_ += dt_ms;
    uint32_t timeout_ms = static_cast<uint32_t>(timeout_sec_) * 1000;
    uint32_t dim_ms = static_cast<uint32_t>(timeout_ms * DIM_FRACTION);

    if (state_ == IdleState::ACTIVE && idle_ms_ >= dim_ms) {
        state_ = IdleState::DIM;
        backlight_set(DIM_BRIGHTNESS);
    }
    if (state_ == IdleState::DIM && idle_ms_ >= timeout_ms) {
        state_ = IdleState::SLEEP;
        // Main loop handles actual sleep entry
    }
}

void IdleTimer::set_timeout(uint16_t sec)
{
    timeout_sec_ = sec;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u16(nvs, NVS_KEY, sec);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

const char* IdleTimer::timeout_label(uint16_t sec)
{
    switch (sec) {
    case 0:   return "Off";
    case 30:  return "30s";
    case 60:  return "1 min";
    case 120: return "2 min";
    case 300: return "5 min";
    case 600: return "10 min";
    default:  return "?";
    }
}

} // namespace os32
