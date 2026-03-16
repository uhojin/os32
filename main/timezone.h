#pragma once

namespace os32 {

struct TimezoneEntry {
    const char *label;   // Display name
    const char *posix;   // POSIX TZ string
};

// Curated list of common timezones with DST rules baked in
static constexpr TimezoneEntry TIMEZONES[] = {
    {"US Eastern",           "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central",           "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain",          "MST7MDT,M3.2.0,M11.1.0"},
    {"US Mountain (no DST)", "MST7"},
    {"US Pacific",           "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska",            "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Hawaii",               "HST10"},
    {"Canada Atlantic",      "AST4ADT,M3.2.0,M11.1.0"},
    {"Canada Saskatchewan",  "CST6"},
    {"UTC",                  "UTC0"},
    {"London",               "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe",       "CET-1CEST,M3.5.0,M10.5.0"},
    {"Eastern Europe",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"India",                "IST-5:30"},
    {"China / Singapore",    "CST-8"},
    {"Korea",                "KST-9"},
    {"Japan",                "JST-9"},
    {"Australia Eastern",    "AEST-10AEDT,M10.1.0,M4.1.0"},
    {"Australia Queensland", "AEST-10"},
    {"Australia Western",    "AWST-8"},
    {"New Zealand",          "NZST-12NZDT,M9.5.0,M4.1.0"},
};

static constexpr int TIMEZONE_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);
static constexpr int TIMEZONE_DEFAULT = 0;  // US Eastern

// Load saved timezone and clock format from NVS, apply timezone
// Call this on boot after NVS is initialized
void timezone_init();

// Timezone
int timezone_get();
void timezone_set(int index);

// 12/24 hour clock format
bool clock_is_24h();
void clock_set_24h(bool use_24h);

} // namespace os32
