#pragma once

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

namespace os32 {

class WifiManager;

enum class SpotifyAuthState : uint8_t {
    NOT_CONFIGURED,
    AUTHORIZING,
    TOKEN_VALID,
    ERROR,
};

struct SpotifyTokens {
    char access_token[512];
    char refresh_token[512];
    int64_t expires_at;  // esp_timer_get_time() microseconds
};

class SpotifyAuth {
public:
    void init();
    void start_auth_flow(WifiManager *wifi);
    void stop_auth_flow();
    SpotifyAuthState state() const;
    bool tokens_valid() const;
    bool token_expired() const;
    void get_access_token(char *buf, size_t len) const;
    void get_auth_url(char *buf, size_t len) const;
    bool refresh_token_sync();
    void update_tokens(const char *access, const char *refresh, int expires_in_sec);
    void forget();

private:
    static esp_err_t handle_root(httpd_req_t *req);
    static esp_err_t handle_callback(httpd_req_t *req);
    static esp_err_t handle_spotify_code(httpd_req_t *req);
    void generate_pkce();
    void save_tokens();
    void load_tokens();
    bool exchange_code(const char *code);

    mutable SemaphoreHandle_t mutex_ = nullptr;
    SpotifyAuthState state_ = SpotifyAuthState::NOT_CONFIGURED;
    SpotifyTokens tokens_ = {};
    httpd_handle_t server_ = nullptr;
    WifiManager *wifi_ = nullptr;
    char code_verifier_[44];    // 43 chars + null
    char code_challenge_[44];   // base64url(SHA256(verifier)) + null
    static SpotifyAuth *s_instance_;
};

} // namespace os32
