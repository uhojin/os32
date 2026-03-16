#include "wifi_manager.h"
#include "captive_portal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "lwip/sockets.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "wifi";

namespace os32 {

static constexpr int MAX_RETRIES = 3;
static constexpr const char *NVS_NAMESPACE = "wifi";
static constexpr const char *NVS_KEY_SSID = "ssid";
static constexpr const char *NVS_KEY_PASS = "pass";

// --- DNS hijack task (runs on core 1) ---

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto *self = static_cast<WifiManager*>(arg);

    // Single buffer for recv and response (modify in-place, append answer)
    uint8_t buf[128];
    sockaddr_in client = {};
    socklen_t client_len;
    const uint8_t ap_ip[] = {192, 168, 4, 1};

    while (self->dns_running_) {
        client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                          reinterpret_cast<sockaddr*>(&client), &client_len);
        if (len < 12) continue;

        // Modify query in-place to become response
        buf[2] = 0x84;  // QR=1, AA=1
        buf[3] = 0x00;  // RCODE=0
        buf[6] = 0x00;  // ANCOUNT = 1
        buf[7] = 0x01;

        // Append A record answer
        int pos = len;
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;  // name pointer
        buf[pos++] = 0x00; buf[pos++] = 0x01;  // type A
        buf[pos++] = 0x00; buf[pos++] = 0x01;  // class IN
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x3C;  // TTL 60
        buf[pos++] = 0x00; buf[pos++] = 0x04;  // rdlength 4
        memcpy(&buf[pos], ap_ip, 4); pos += 4;

        sendto(sock, buf, pos, 0,
               reinterpret_cast<sockaddr*>(&client), client_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS task stopped");
    vTaskDelete(nullptr);
}

// --- Init ---

void WifiManager::init()
{
    mutex_ = xSemaphoreCreateMutex();

    // Init NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Init network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, this, nullptr));

    load_credentials();
    ESP_LOGI(TAG, "WiFi initialized, credentials %s",
             credentials_loaded_ ? "found" : "not found");
}

void WifiManager::event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    auto *self = static_cast<WifiManager*>(arg);
    if (base == WIFI_EVENT) {
        self->on_wifi_event(id, data);
    } else if (base == IP_EVENT) {
        self->on_ip_event(id, data);
    }
}

void WifiManager::on_wifi_event(int32_t id, void *data)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (state_ == WifiState::CONNECTING) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (state_ == WifiState::CONNECTING) {
            if (retry_count_ < MAX_RETRIES) {
                retry_count_++;
                ESP_LOGI(TAG, "Retry %d/%d", retry_count_, MAX_RETRIES);
                esp_wifi_connect();
            } else {
                state_ = WifiState::FAILED;
                ESP_LOGW(TAG, "Connection failed");
            }
        } else if (state_ == WifiState::CONNECTED) {
            state_ = WifiState::FAILED;
            ESP_LOGW(TAG, "Disconnected");
        }
        // Ignore disconnects in other states (IDLE, AP_ACTIVE, FAILED)
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Client connected to AP");
        break;
    case WIFI_EVENT_SCAN_DONE: {
        scan_count_ = 20;
        esp_wifi_scan_get_ap_records(&scan_count_, scan_buf_);
        scan_done_ = true;
        ESP_LOGI(TAG, "Scan complete: %d networks", scan_count_);
        break;
    }
    default:
        break;
    }
    xSemaphoreGive(mutex_);
}

void WifiManager::on_ip_event(int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t*>(data);
        xSemaphoreTake(mutex_, portMAX_DELAY);
        state_ = WifiState::CONNECTED;
        snprintf(ip_str_, sizeof(ip_str_), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected, IP: %s", ip_str_);
        xSemaphoreGive(mutex_);
        sync_ntp();
    }
}

// --- NVS ---

void WifiManager::load_credentials()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;

    size_t ssid_len = sizeof(ssid_);
    size_t pass_len = sizeof(password_);
    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, ssid_, &ssid_len) == ESP_OK)
           && (nvs_get_str(nvs, NVS_KEY_PASS, password_, &pass_len) == ESP_OK);
    nvs_close(nvs);

    credentials_loaded_ = ok && ssid_[0] != '\0';
}

void WifiManager::save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, NVS_KEY_PASS, pass));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    strncpy(password_, pass, sizeof(password_) - 1);
    credentials_loaded_ = true;
    ESP_LOGI(TAG, "Credentials saved for '%s'", ssid_);
}

void WifiManager::forget_credentials()
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ssid_[0] = '\0';
    password_[0] = '\0';
    credentials_loaded_ = false;
    ESP_LOGI(TAG, "Credentials erased");
}

// --- STA ---

bool WifiManager::has_saved_credentials()
{
    return credentials_loaded_;
}

void WifiManager::connect_saved()
{
    if (!credentials_loaded_) return;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_ = WifiState::CONNECTING;
    retry_count_ = 0;
    xSemaphoreGive(mutex_);

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid_, sizeof(cfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(cfg.sta.password), password_, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to '%s'", ssid_);
}

void WifiManager::disconnect()
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_ = WifiState::IDLE;
    ip_str_[0] = '\0';
    xSemaphoreGive(mutex_);
}

// --- AP ---

void WifiManager::generate_ap_credentials()
{
    strncpy(ap_ssid_, "os32-setup", sizeof(ap_ssid_));

    static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) {
        ap_password_[i] = chars[esp_random() % (sizeof(chars) - 1)];
    }
    ap_password_[8] = '\0';
}

void WifiManager::start_ap()
{
    generate_ap_credentials();

    wifi_config_t cfg = {};
    strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ap_ssid_, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(ap_ssid_);
    strncpy(reinterpret_cast<char*>(cfg.ap.password), ap_password_, sizeof(cfg.ap.password));
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    // APSTA so scanning works while serving the portal
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_ = WifiState::AP_ACTIVE;
    xSemaphoreGive(mutex_);

    // Start captive portal HTTP server
    if (!portal_) {
        portal_ = new CaptivePortal();
    }
    portal_->start(this);

    // Start DNS hijack on core 1
    dns_running_ = true;
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, this, 5, &dns_task_, 1);

    ESP_LOGI(TAG, "AP started: %s / %s", ap_ssid_, ap_password_);
}

void WifiManager::stop_ap(bool reconnect)
{
    // Stop DNS task
    if (dns_task_) {
        dns_running_ = false;
        // Task will exit within 5s (recv timeout), then delete itself
        dns_task_ = nullptr;
    }

    if (portal_) {
        portal_->stop();
    }
    esp_wifi_stop();

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (state_ == WifiState::AP_ACTIVE) {
        state_ = WifiState::IDLE;
    }
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "AP stopped");

    if (reconnect && credentials_loaded_) {
        connect_saved();
    }
}

// --- Scan ---

void WifiManager::scan_networks()
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    scan_done_ = false;
    scan_count_ = 0;
    xSemaphoreGive(mutex_);

    // Non-blocking scan — results arrive via WIFI_EVENT_SCAN_DONE
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_wifi_scan_start(&scan_cfg, false);
}

static void connect_task(void *arg)
{
    auto *self = static_cast<WifiManager*>(arg);
    // Give HTTP response time to flush
    vTaskDelay(pdMS_TO_TICKS(1000));
    self->stop_ap(false);
    self->connect_saved();
    vTaskDelete(nullptr);
}

bool WifiManager::save_and_connect(const char *ssid, const char *password)
{
    save_credentials(ssid, password);
    // Defer AP teardown to a separate task so the HTTP response sends first
    xTaskCreate(connect_task, "wifi_conn", 4096, this, 5, nullptr);
    return true;
}

// --- Getters ---

WifiState WifiManager::state() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    auto s = state_;
    xSemaphoreGive(mutex_);
    return s;
}

void WifiManager::get_ssid(char *buf, size_t len) const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(buf, ssid_, len);
    buf[len - 1] = '\0';
    xSemaphoreGive(mutex_);
}

void WifiManager::get_ip(char *buf, size_t len) const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(buf, ip_str_, len);
    buf[len - 1] = '\0';
    xSemaphoreGive(mutex_);
}

void WifiManager::get_ap_ssid(char *buf, size_t len) const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(buf, ap_ssid_, len);
    buf[len - 1] = '\0';
    xSemaphoreGive(mutex_);
}

void WifiManager::get_ap_password(char *buf, size_t len) const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    strncpy(buf, ap_password_, len);
    buf[len - 1] = '\0';
    xSemaphoreGive(mutex_);
}

int8_t WifiManager::rssi() const
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return info.rssi;
    }
    return 0;
}

int WifiManager::scan_count() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    int n = scan_count_;
    xSemaphoreGive(mutex_);
    return n;
}

const wifi_ap_record_t* WifiManager::scan_results() const
{
    return scan_buf_;
}

bool WifiManager::is_scan_done() const
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool done = scan_done_;
    xSemaphoreGive(mutex_);
    return done;
}

// --- NTP ---

void WifiManager::sync_ntp()
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "NTP already running");
        return;
    }
    ESP_LOGI(TAG, "Starting NTP sync");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

} // namespace os32
