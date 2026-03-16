#pragma once

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstdint>

namespace os32 {

enum class WifiState : uint8_t {
    IDLE,
    CONNECTING,
    CONNECTED,
    AP_ACTIVE,
    FAILED,
};

class CaptivePortal;

class WifiManager {
public:
    void init();

    // STA
    bool has_saved_credentials();
    void connect_saved();
    void disconnect();
    void forget_credentials();

    // AP provisioning
    void start_ap();
    void stop_ap(bool reconnect = true);

    // Called from captive portal REST handlers
    void scan_networks();
    bool save_and_connect(const char *ssid, const char *password);

    // Thread-safe state access
    WifiState state() const;
    void get_ssid(char *buf, size_t len) const;
    void get_ip(char *buf, size_t len) const;
    void get_ap_ssid(char *buf, size_t len) const;
    void get_ap_password(char *buf, size_t len) const;
    int8_t rssi() const;

    // Scan results (non-blocking)
    int scan_count() const;
    const wifi_ap_record_t* scan_results() const;
    bool is_scan_done() const;

    // DNS task needs access
    volatile bool dns_running_ = false;

private:
    static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
    void on_wifi_event(int32_t id, void *data);
    void on_ip_event(int32_t id, void *data);
    void generate_ap_credentials();
    void load_credentials();
    void save_credentials(const char *ssid, const char *pass);
    void sync_ntp();

    mutable SemaphoreHandle_t mutex_ = nullptr;
    WifiState state_ = WifiState::IDLE;
    int retry_count_ = 0;

    char ssid_[33] = {};
    char password_[65] = {};
    char ap_ssid_[17] = {};
    char ap_password_[9] = {};
    char ip_str_[16] = {};

    wifi_ap_record_t scan_buf_[20] = {};
    uint16_t scan_count_ = 0;
    bool scan_done_ = false;

    CaptivePortal *portal_ = nullptr;
    TaskHandle_t dns_task_ = nullptr;
    bool credentials_loaded_ = false;
};

} // namespace os32
