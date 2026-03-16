#pragma once

#include "esp_http_server.h"

namespace os32 {

class SdManager;

class FileServer {
public:
    void start(SdManager *sd);
    void stop();
    bool running() const { return server_ != nullptr; }

private:
    httpd_handle_t server_ = nullptr;
};

} // namespace os32
