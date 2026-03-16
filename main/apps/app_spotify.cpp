#include "app_spotify.h"
#include "os32.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace os32 {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SpotifyApp::on_enter(lv_obj_t *screen)
{
    screen_ = screen;
    auth_.init();
    client_.init(&auth_);
    first_poll_ = true;
    poll_accum_ms_ = 0;
    progress_local_ms_ = 0;
    right_held_skip_ = true;  // suppress play/pause from launcher RIGHT release

    if (wifi_->state() != WifiState::CONNECTED) {
        page_ = Page::NO_WIFI;
    } else if (auth_.state() == SpotifyAuthState::NOT_CONFIGURED) {
        page_ = Page::SETUP;
    } else {
        page_ = Page::NOW_PLAYING;
        poll_accum_ms_ = 4000;  // trigger immediate poll
    }

    build_page();

    // Restore cached art if available
    if (page_ == Page::NOW_PLAYING && art_thumb_.valid()) {
        if (current_track_.valid) update_now_playing();
        show_art();
    }
}

void SpotifyApp::on_exit()
{
    auth_.stop_auth_flow();
    // Keep art_thumb_ and last_album_id_ cached for re-entry
    page_ = Page::NOW_PLAYING;
    screen_ = nullptr;
    info_label_ = nullptr;
    qr_ = nullptr;
    art_canvas_ = nullptr;
    title_label_ = nullptr;
    artist_label_ = nullptr;
    progress_bar_ = nullptr;
    time_label_ = nullptr;
    duration_label_ = nullptr;
    status_label_ = nullptr;
}

void SpotifyApp::on_update(uint32_t dt_ms)
{
    // Check WiFi drop
    if (wifi_->state() != WifiState::CONNECTED && page_ != Page::NO_WIFI) {
        page_ = Page::NO_WIFI;
        build_page();
        return;
    }

    // Auth flow completed?
    if (page_ == Page::SETUP && auth_.state() == SpotifyAuthState::TOKEN_VALID) {
        auth_.stop_auth_flow();
        page_ = Page::NOW_PLAYING;
        first_poll_ = true;
        poll_accum_ms_ = 4000;  // trigger immediate poll
        build_page();
        return;
    }

    if (page_ != Page::NOW_PLAYING) return;

    // Force immediate poll after waking from sleep (dt > 5s means we were idle)
    if (dt_ms > 5000) poll_accum_ms_ = 4000;

    // Poll timer
    poll_accum_ms_ += dt_ms;
    if ((first_poll_ && poll_accum_ms_ >= 500) || poll_accum_ms_ >= 4000) {
        if (client_.state() == SpotifyClientState::IDLE ||
            client_.state() == SpotifyClientState::DONE_TRACK ||
            client_.state() == SpotifyClientState::DONE_ART ||
            client_.state() == SpotifyClientState::ERROR) {
            client_.poll();
            poll_accum_ms_ = 0;
            first_poll_ = false;
        }
    }

    // Handle client results
    auto cstate = client_.state();

    if (cstate == SpotifyClientState::DONE_ART) {
        // Art comes with track data
        current_track_ = client_.take_track();
        art_thumb_.release();
        art_thumb_ = client_.take_art();
        strncpy(last_album_id_, current_track_.album_id, sizeof(last_album_id_) - 1);
        progress_local_ms_ = current_track_.progress_ms;
        update_now_playing();
        show_art();
    } else if (cstate == SpotifyClientState::DONE_TRACK) {
        current_track_ = client_.take_track();
        progress_local_ms_ = current_track_.progress_ms;
        update_now_playing();
    } else if (cstate == SpotifyClientState::NO_DEVICE) {
        client_.acknowledge();
        page_ = Page::NO_DEVICE;
        build_page();
        return;
    } else if (cstate == SpotifyClientState::AUTH_EXPIRED) {
        client_.acknowledge();
        auth_.forget();
        page_ = Page::SETUP;
        build_page();
        return;
    }

    // Interpolate progress bar
    if (current_track_.valid && current_track_.is_playing && progress_bar_) {
        progress_local_ms_ += dt_ms;
        if (progress_local_ms_ > current_track_.duration_ms)
            progress_local_ms_ = current_track_.duration_ms;
        lv_bar_set_value(progress_bar_, progress_local_ms_, LV_ANIM_OFF);

        if (time_label_) {
            uint32_t cur_s = progress_local_ms_ / 1000;
            char buf[10];
            snprintf(buf, sizeof(buf), "%u:%02u",
                     (unsigned)(cur_s / 60), (unsigned)(cur_s % 60));
            lv_label_set_text(time_label_, buf);
        }
    }
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------

bool SpotifyApp::on_button(ButtonId btn, ButtonEvent event)
{
    if (page_ == Page::NO_WIFI) return false;

    if (page_ == Page::SETUP) {
        // LEFT = cancel (handled by AppManager as unconsumed)
        return false;
    }

    if (page_ == Page::NO_DEVICE || page_ == Page::ERROR) {
        if (btn == ButtonId::RIGHT && event == ButtonEvent::PRESS) {
            page_ = Page::NOW_PLAYING;
            first_poll_ = true;
            poll_accum_ms_ = 4000;
            build_page();
            return true;
        }
        return false;
    }

    // NOW_PLAYING controls
    // RIGHT tap = play/pause, RIGHT hold = next track
    // UP/DOWN tap = volume step, hold = volume repeat
    // LEFT = back (unconsumed, handled by AppManager)
    if (btn == ButtonId::RIGHT) {
        if (event == ButtonEvent::PRESS) {
            right_held_skip_ = false;
        } else if (event == ButtonEvent::REPEAT) {
            if (!right_held_skip_) {
                right_held_skip_ = true;
                client_.post_command(SpotifyCmd::NEXT);
            }
        } else if (event == ButtonEvent::RELEASE) {
            if (!right_held_skip_) {
                client_.post_command(current_track_.is_playing
                    ? SpotifyCmd::PAUSE : SpotifyCmd::PLAY);
                current_track_.is_playing = !current_track_.is_playing;
                if (status_label_)
                    lv_label_set_text(status_label_,
                        current_track_.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            }
        }
        return true;
    }

    if (btn == ButtonId::UP || btn == ButtonId::DOWN) {
        if (event == ButtonEvent::PRESS || event == ButtonEvent::REPEAT) {
            client_.post_command(btn == ButtonId::UP
                ? SpotifyCmd::VOLUME_UP : SpotifyCmd::VOLUME_DOWN);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Page building
// ---------------------------------------------------------------------------

void SpotifyApp::build_page()
{
    if (!screen_) return;

    // Clear screen content (below header), keep header row (y < CONTENT_Y)
    uint32_t count = lv_obj_get_child_count(screen_);
    for (int i = static_cast<int>(count) - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(screen_, i);
        if (lv_obj_get_y(child) < CONTENT_Y)
            continue;  // keep header
        lv_obj_delete(child);
    }

    // Reset widget pointers
    info_label_ = nullptr;
    qr_ = nullptr;
    art_canvas_ = nullptr;
    title_label_ = nullptr;
    artist_label_ = nullptr;
    progress_bar_ = nullptr;
    time_label_ = nullptr;
    duration_label_ = nullptr;
    status_label_ = nullptr;

    switch (page_) {
        case Page::SETUP: {
            auth_.start_auth_flow(wifi_);

            char url[512];
            auth_.get_auth_url(url, sizeof(url));

            // QR code centered
            qr_ = lv_qrcode_create(screen_);
            lv_qrcode_set_size(qr_, 120);
            lv_qrcode_set_dark_color(qr_, color::fg());
            lv_qrcode_set_light_color(qr_, color::bg());
            lv_qrcode_update(qr_, url, strlen(url));
            lv_obj_align(qr_, LV_ALIGN_TOP_MID, 0, CONTENT_Y);

            // Instructions below QR
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_small(), 0);
            lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
            lv_label_set_text(info_label_, "Scan to connect Spotify");
            lv_obj_align(info_label_, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 126);

            // Hostname hint
            auto *ip_label = lv_label_create(screen_);
            lv_obj_set_style_text_font(ip_label, font_small(), 0);
            lv_obj_set_style_text_color(ip_label, color::fg_dim(), 0);
            lv_label_set_text(ip_label, "or visit os32.local:8888");
            lv_obj_align(ip_label, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 140);
            break;
        }

        case Page::NOW_PLAYING: {
            constexpr int ART_SIZE = 120;
            constexpr int TEXT_W = LCD_H_RES - PAD * 2;

            // Art placeholder centered
            art_canvas_ = lv_obj_create(screen_);
            lv_obj_remove_style_all(art_canvas_);
            lv_obj_set_size(art_canvas_, ART_SIZE, ART_SIZE);
            lv_obj_align(art_canvas_, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
            lv_obj_set_style_bg_color(art_canvas_, color::bg2(), 0);
            lv_obj_set_style_bg_opa(art_canvas_, LV_OPA_COVER, 0);

            constexpr int TEXT_Y = CONTENT_Y + ART_SIZE + 4;

            // Track title — centered
            title_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(title_label_, font_bold(), 0);
            lv_obj_set_style_text_color(title_label_, color::fg(), 0);
            lv_obj_set_width(title_label_, TEXT_W);
            lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, TEXT_Y);
            lv_label_set_text(title_label_, "---");

            // Artist — centered
            artist_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(artist_label_, font_mono(), 0);
            lv_obj_set_style_text_color(artist_label_, color::fg_dim(), 0);
            lv_obj_set_width(artist_label_, TEXT_W);
            lv_obj_set_style_text_align(artist_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(artist_label_, LV_ALIGN_TOP_MID, 0, TEXT_Y + FONT_H + 2);
            lv_label_set_text(artist_label_, "---");

            // Layout from bottom up: bar → time → play/pause
            constexpr int BAR_Y = LCD_V_RES - 16;  // progress bar near bottom
            constexpr int TIME_Y = BAR_Y - 14;      // time labels above bar
            constexpr int ARTIST_BOTTOM = TEXT_Y + FONT_H + 2 + FONT_H;
            constexpr int PLAY_Y = (ARTIST_BOTTOM + TIME_Y) / 2 - 3;  // centered between artist and time

            // Progress bar — full width at bottom
            progress_bar_ = lv_bar_create(screen_);
            lv_obj_set_size(progress_bar_, LCD_H_RES - PAD * 2, 4);
            lv_obj_set_pos(progress_bar_, PAD, BAR_Y);
            lv_bar_set_range(progress_bar_, 0, 1000);
            lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(progress_bar_, color::bg2(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(progress_bar_, color::green(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);

            // Elapsed time left, duration right — above bar
            time_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(time_label_, font_small(), 0);
            lv_obj_set_style_text_color(time_label_, color::fg_dim(), 0);
            lv_obj_set_pos(time_label_, PAD, TIME_Y);
            lv_label_set_text(time_label_, "");

            duration_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(duration_label_, font_small(), 0);
            lv_obj_set_style_text_color(duration_label_, color::fg_dim(), 0);
            lv_obj_align(duration_label_, LV_ALIGN_TOP_RIGHT, -PAD, TIME_Y);
            lv_label_set_text(duration_label_, "");

            // Play/pause centered above time
            status_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(status_label_, color::green(), 0);
            lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, PLAY_Y);
            lv_label_set_text(status_label_, LV_SYMBOL_PLAY);

            if (current_track_.valid)
                update_now_playing();
            break;
        }

        case Page::NO_WIFI:
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_mono(), 0);
            lv_obj_set_style_text_color(info_label_, color::fg(), 0);
            lv_label_set_text(info_label_,
                "WiFi not connected.\n\n"
                "Go to Settings > WiFi\n"
                "to connect first.");
            lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
            break;

        case Page::NO_DEVICE:
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_mono(), 0);
            lv_obj_set_style_text_color(info_label_, color::fg(), 0);
            lv_label_set_text(info_label_,
                "No active Spotify\n"
                "device found.\n\n"
                "Open Spotify on your\n"
                "phone or computer.\n\n"
                "[RIGHT] Retry");
            lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
            break;

        case Page::ERROR:
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_mono(), 0);
            lv_obj_set_style_text_color(info_label_, color::red(), 0);
            lv_label_set_text(info_label_,
                "Spotify error.\n\n"
                "[RIGHT] Retry");
            lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
            break;
    }
}

// ---------------------------------------------------------------------------
// Now playing updates (in-place, no rebuild)
// ---------------------------------------------------------------------------

void SpotifyApp::update_now_playing()
{
    if (!title_label_) return;

    if (!current_track_.valid) {
        lv_label_set_text(title_label_, "Nothing playing");
        lv_label_set_text(artist_label_, "");
        lv_label_set_text(status_label_, "");
        if (progress_bar_) lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
        if (time_label_) lv_label_set_text(time_label_, "");
        if (duration_label_) lv_label_set_text(duration_label_, "");
        return;
    }

    lv_label_set_text(title_label_, current_track_.title);
    lv_label_set_text(artist_label_, current_track_.artist);
    lv_label_set_text(status_label_,
        current_track_.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (progress_bar_ && current_track_.duration_ms > 0) {
        lv_bar_set_range(progress_bar_, 0, current_track_.duration_ms);
        lv_bar_set_value(progress_bar_, current_track_.progress_ms, LV_ANIM_OFF);
    }

    if (time_label_) {
        uint32_t cur_s = current_track_.progress_ms / 1000;
        char buf[10];
        snprintf(buf, sizeof(buf), "%u:%02u",
                 (unsigned)(cur_s / 60), (unsigned)(cur_s % 60));
        lv_label_set_text(time_label_, buf);
    }
    if (duration_label_) {
        uint32_t dur_s = current_track_.duration_ms / 1000;
        char buf[10];
        snprintf(buf, sizeof(buf), "%u:%02u",
                 (unsigned)(dur_s / 60), (unsigned)(dur_s % 60));
        lv_label_set_text(duration_label_, buf);
    }
}

// ---------------------------------------------------------------------------
// Album art display
// ---------------------------------------------------------------------------

// Extract accent color from RGB565 pixels — hue histogram binning
static lv_color_t extract_accent(const uint16_t *pixels, int count)
{
    // 12 hue bins (30° each), track pixel count and RGB accumulators per bin
    static constexpr int NUM_BINS = 12;
    struct HueBin {
        uint32_t count;
        uint32_t r, g, b;
    };
    HueBin bins[NUM_BINS] = {};

    for (int i = 0; i < count; i++) {
        uint16_t px = pixels[i];
        uint8_t r = (px >> 11) << 3;
        uint8_t g = ((px >> 5) & 0x3F) << 2;
        uint8_t b = (px & 0x1F) << 3;

        uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        uint8_t delta = mx - mn;

        // Skip dark or desaturated pixels
        if (mx < 30 || delta < 25) continue;

        // Compute hue (0-359)
        int hue;
        if (delta == 0) continue;
        if (mx == r)      hue = 60 * (static_cast<int>(g) - b) / delta;
        else if (mx == g) hue = 120 + 60 * (static_cast<int>(b) - r) / delta;
        else              hue = 240 + 60 * (static_cast<int>(r) - g) / delta;
        if (hue < 0) hue += 360;

        int bin = (hue * NUM_BINS) / 360;
        if (bin >= NUM_BINS) bin = NUM_BINS - 1;

        bins[bin].count++;
        bins[bin].r += r;
        bins[bin].g += g;
        bins[bin].b += b;
    }

    // Find the most populated bin
    int best = -1;
    uint32_t best_count = 0;
    for (int i = 0; i < NUM_BINS; i++) {
        if (bins[i].count > best_count) {
            best_count = bins[i].count;
            best = i;
        }
    }

    if (best < 0) return lv_color_make(0xb8, 0xbb, 0x26);  // fallback green

    // Average RGB of the winning bin
    uint8_t fr = bins[best].r / bins[best].count;
    uint8_t fg = bins[best].g / bins[best].count;
    uint8_t fb = bins[best].b / bins[best].count;

    // Boost brightness so it pops on the dark background
    uint8_t mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
    if (mx > 0 && mx < 180) {
        float boost = 180.0f / mx;
        fr = static_cast<uint8_t>(fr * boost > 255 ? 255 : fr * boost);
        fg = static_cast<uint8_t>(fg * boost > 255 ? 255 : fg * boost);
        fb = static_cast<uint8_t>(fb * boost > 255 ? 255 : fb * boost);
    }

    return lv_color_make(fr, fg, fb);
}

void SpotifyApp::show_art()
{
    if (!art_canvas_ || !art_thumb_.valid()) return;

    // Delete the placeholder and create a canvas in its place
    lv_obj_delete(art_canvas_);

    lv_obj_t *canvas = lv_canvas_create(screen_);

    // Set up draw buffer from our thumbnail pixels
    static lv_draw_buf_t art_draw_buf;
    lv_memzero(&art_draw_buf, sizeof(art_draw_buf));
    art_draw_buf.header.w = art_thumb_.width;
    art_draw_buf.header.h = art_thumb_.height;
    art_draw_buf.header.cf = LV_COLOR_FORMAT_RGB565;
    art_draw_buf.header.stride = art_thumb_.width * 2;
    art_draw_buf.data = reinterpret_cast<uint8_t *>(art_thumb_.pixels);
    art_draw_buf.data_size = art_thumb_.width * art_thumb_.height * 2;

    lv_canvas_set_draw_buf(canvas, &art_draw_buf);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, CONTENT_Y);

    art_canvas_ = canvas;

    // Extract accent color and apply to progress bar + play/pause
    lv_color_t accent = extract_accent(art_thumb_.pixels,
                                        art_thumb_.width * art_thumb_.height);
    if (progress_bar_)
        lv_obj_set_style_bg_color(progress_bar_, accent, LV_PART_INDICATOR);
    if (status_label_)
        lv_obj_set_style_text_color(status_label_, accent, 0);
}

void SpotifyApp::free_art()
{
    art_thumb_.release();
}

// ---------------------------------------------------------------------------
// Status text (1602 LCD)
// ---------------------------------------------------------------------------

void SpotifyApp::get_status_text(char *line1, char *line2)
{
    if (!current_track_.valid) {
        snprintf(line1, 17, "%-16s", "Spotify");
        snprintf(line2, 17, "%-16s", "Not playing");
        lcd_scroll_pos_ = 0;
        return;
    }

    // Line 1: scrolling "title - artist"
    char combined[384];
    snprintf(combined, sizeof(combined), "%s - %s",
             current_track_.title, current_track_.artist);
    size_t len = strlen(combined);

    if (len <= 16) {
        snprintf(line1, 17, "%-16.16s", combined);
    } else {
        int offset = lcd_scroll_pos_ % (static_cast<int>(len) + 4);
        char padded[384 * 2 + 8];
        snprintf(padded, sizeof(padded), "%s    %s", combined, combined);
        snprintf(line1, 17, "%.16s", padded + offset);
        lcd_scroll_pos_++;
    }

    // Line 2: progress bar [####............]
    if (current_track_.duration_ms > 0) {
        int filled = static_cast<int>(
            static_cast<uint64_t>(progress_local_ms_) * 16 / current_track_.duration_ms);
        if (filled > 16) filled = 16;
        for (int i = 0; i < 16; i++)
            line2[i] = (i < filled) ? '\xFF' : '\xA5';
        line2[16] = '\0';
    } else {
        snprintf(line2, 17, "%-16s", "");
    }
}

void SpotifyApp::get_header_text(char *buf, size_t len) const
{
    switch (page_) {
        case Page::SETUP:
            snprintf(buf, len, "Spotify > Setup");
            break;
        case Page::NO_WIFI:
            snprintf(buf, len, "Spotify > No WiFi");
            break;
        case Page::NOW_PLAYING:
            snprintf(buf, len, current_track_.is_playing ? "Now Playing" : "Spotify");
            break;
        default:
            snprintf(buf, len, "Spotify");
            break;
    }
}

} // namespace os32
