#include "timezone.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <cstdlib>
#include <ctime>

static const char *TAG = "timezone";
static constexpr const char *NVS_NS = "timezone";
static constexpr const char *NVS_KEY = "tz_idx";
static constexpr const char *NVS_KEY_24H = "clock_24h";

namespace os32 {

static int current_index_ = TIMEZONE_DEFAULT;
static bool use_24h_ = true;

static void apply_tz(int index)
{
    if (index < 0 || index >= TIMEZONE_COUNT) index = TIMEZONE_DEFAULT;
    setenv("TZ", TIMEZONES[index].posix, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone: %s (%s)", TIMEZONES[index].label, TIMEZONES[index].posix);
}

void timezone_init()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t idx = TIMEZONE_DEFAULT;
        if (nvs_get_u8(nvs, NVS_KEY, &idx) == ESP_OK && idx < TIMEZONE_COUNT) {
            current_index_ = idx;
        }
        uint8_t h24 = 1;
        if (nvs_get_u8(nvs, NVS_KEY_24H, &h24) == ESP_OK) {
            use_24h_ = h24 != 0;
        }
        nvs_close(nvs);
    }
    apply_tz(current_index_);
}

int timezone_get()
{
    return current_index_;
}

void timezone_set(int index)
{
    if (index < 0 || index >= TIMEZONE_COUNT) return;
    current_index_ = index;
    apply_tz(index);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY, static_cast<uint8_t>(index));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool clock_is_24h()
{
    return use_24h_;
}

void clock_set_24h(bool use_24h)
{
    use_24h_ = use_24h;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_24H, use_24h ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

} // namespace os32
