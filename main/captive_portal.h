#pragma once

#include "esp_http_server.h"

namespace os32 {

class WifiManager;

class CaptivePortal {
public:
    void start(WifiManager *wifi_mgr);
    void stop();

private:
    static esp_err_t handle_root(httpd_req_t *req);
    static esp_err_t handle_scan(httpd_req_t *req);
    static esp_err_t handle_connect(httpd_req_t *req);
    static esp_err_t handle_status(httpd_req_t *req);
    static esp_err_t handle_redirect(httpd_req_t *req);

    httpd_handle_t server_ = nullptr;
    WifiManager *wifi_mgr_ = nullptr;
};

} // namespace os32
