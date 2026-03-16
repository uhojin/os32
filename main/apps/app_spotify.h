#pragma once

#include "app.h"
#include "spotify_auth.h"
#include "spotify_client.h"
#include "thumbnail.h"

namespace os32 {

class WifiManager;

class SpotifyApp : public App {
public:
    explicit SpotifyApp(WifiManager *wifi) : wifi_(wifi) {}
    const char* name() const override { return "Spotify"; }
    void on_enter(lv_obj_t *screen) override;
    void on_update(uint32_t dt_ms) override;
    void on_exit() override;
    bool on_button(ButtonId btn, ButtonEvent event) override;
    void get_status_text(char *line1, char *line2) override;
    void get_header_text(char *buf, std::size_t len) const override;

private:
    enum class Page {
        NOW_PLAYING,
        SETUP,
        NO_WIFI,
        NO_DEVICE,
        ERROR,
    };

    void build_page();
    void update_now_playing();
    void show_art();
    void free_art();

    WifiManager *wifi_;
    SpotifyAuth auth_;
    SpotifyClient client_;

    Page page_ = Page::NOW_PLAYING;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *info_label_ = nullptr;
    lv_obj_t *qr_ = nullptr;

    // Now playing widgets
    lv_obj_t *art_canvas_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *artist_label_ = nullptr;
    lv_obj_t *progress_bar_ = nullptr;
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *duration_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;

    Thumbnail art_thumb_;
    SpotifyTrack current_track_ = {};
    char last_album_id_[32] = {};

    uint32_t poll_accum_ms_ = 0;
    uint32_t progress_local_ms_ = 0;
    bool first_poll_ = true;
    int lcd_scroll_pos_ = 0;

    bool right_held_skip_ = false;
};

} // namespace os32
