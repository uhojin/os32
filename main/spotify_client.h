#pragma once

#include "thumbnail.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace os32 {

class SpotifyAuth;

struct SpotifyTrack {
    char title[128];
    char artist[128];
    char album[64];
    char album_id[32];
    char art_url[256];
    uint32_t duration_ms;
    uint32_t progress_ms;
    bool is_playing;
    bool valid;
};

enum class SpotifyCmd : uint8_t {
    NONE, PLAY, PAUSE, NEXT, PREV,
    VOLUME_UP, VOLUME_DOWN,
    POLL_NOW,
};

enum class SpotifyClientState : uint8_t {
    IDLE,
    BUSY,
    DONE_TRACK,
    DONE_ART,
    ERROR,
    NO_DEVICE,
    AUTH_EXPIRED,
};

class SpotifyClient {
public:
    void init(SpotifyAuth *auth);
    void post_command(SpotifyCmd cmd);
    void poll();
    void clear_art_cache() { memset(last_album_id_, 0, sizeof(last_album_id_)); }
    void acknowledge() { state_ = SpotifyClientState::IDLE; }
    SpotifyClientState state() const { return state_; }
    SpotifyTrack take_track();
    Thumbnail take_art();
    int volume_percent() const { return volume_; }

private:
    static void task_func(void *arg);
    int do_api_call(const char *method, const char *url,
                    const char *body, char *resp, size_t resp_size);
    bool parse_currently_playing(const char *json);
    Thumbnail fetch_and_decode_art(const char *url, int max_w, int max_h);

    SpotifyAuth *auth_ = nullptr;
    volatile SpotifyClientState state_ = SpotifyClientState::IDLE;
    volatile SpotifyCmd pending_cmd_ = SpotifyCmd::NONE;
    SpotifyTrack result_track_ = {};
    Thumbnail result_art_ = {};
    char last_album_id_[32] = {};
    int volume_ = 50;
};

} // namespace os32
