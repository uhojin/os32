#include "app_files.h"
#include "os32.h"
#include "wifi_manager.h"
#include "file_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "files";

static void join_path(char *out, size_t len, const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen >= len) {
        snprintf(out, len, "%s", dir);
        return;
    }
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
}

namespace os32 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FilesApp::FileKind FilesApp::file_kind(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return FileKind::OTHER;
    dot++;
    if (strcasecmp(dot, "bmp") == 0 || strcasecmp(dot, "jpg") == 0 ||
        strcasecmp(dot, "jpeg") == 0)
        return FileKind::IMAGE;
    if (strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "log") == 0 ||
        strcasecmp(dot, "json") == 0 || strcasecmp(dot, "csv") == 0 ||
        strcasecmp(dot, "cfg") == 0 || strcasecmp(dot, "ini") == 0)
        return FileKind::TEXT;
    return FileKind::OTHER;
}

static void format_size(char *buf, size_t len, uint32_t bytes)
{
    if (bytes < 1024) {
        snprintf(buf, len, "%lu B", (unsigned long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, len, "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, len, "%.1f MB", bytes / (1024.0 * 1024.0));
    }
}

void FilesApp::free_preview()
{
    loader_.cancel();

    if (thumb_canvas_) {
        lv_obj_delete(thumb_canvas_);
        thumb_canvas_ = nullptr;
    }
    thumb_.release();

    if (text_container_) {
        lv_obj_delete(text_container_);
        text_container_ = nullptr;
    }
    if (text_buf_) {
        heap_caps_free(text_buf_);
        text_buf_ = nullptr;
    }
}

void FilesApp::show_loading()
{
    spinner_ = lv_spinner_create(screen_);
    lv_spinner_set_anim_params(spinner_, 800, 270);
    lv_obj_set_size(spinner_, 36, 36);
    lv_obj_align(spinner_, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_arc_color(spinner_, color::bg2(), 0);
    lv_obj_set_style_arc_color(spinner_, color::fg_dim(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner_, 3, 0);
    lv_obj_set_style_arc_width(spinner_, 3, LV_PART_INDICATOR);

    info_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(info_label_, font_mono(), 0);
    lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
    lv_obj_align(info_label_, LV_ALIGN_CENTER, 0, 20);
    lv_label_set_text(info_label_, "Loading...");
}

void FilesApp::complete_file_info()
{
    auto &e = entries_[selected_idx_];
    FileKind kind = e.is_dir ? FileKind::OTHER : file_kind(e.name);
    bool previewable = (kind == FileKind::IMAGE || kind == FileKind::TEXT);

    int text_y = CONTENT_Y;

    if (thumb_.valid()) {
        int thumb_x = (LCD_H_RES - thumb_.width) / 2;
        thumb_canvas_ = lv_canvas_create(screen_);
        lv_canvas_set_buffer(thumb_canvas_, thumb_.pixels,
                             thumb_.width, thumb_.height,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(thumb_canvas_, thumb_x, CONTENT_Y);
        lv_obj_set_style_border_width(thumb_canvas_, 1, 0);
        lv_obj_set_style_border_color(thumb_canvas_, color::bg2(), 0);
        text_y = CONTENT_Y + thumb_.height + 6;
    }

    info_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(info_label_, font_mono(), 0);
    lv_obj_set_style_text_color(info_label_, color::fg(), 0);
    lv_obj_set_pos(info_label_, PAD, text_y);

    char buf[256];
    if (e.is_dir) {
        snprintf(buf, sizeof(buf),
            "Name: %s\n"
            "Type: Directory\n"
            "\n"
            "[LEFT] Back\n"
            "[RIGHT] Delete",
            e.name);
    } else {
        char sz[16];
        format_size(sz, sizeof(sz), e.size);

        if (thumb_.valid()) {
            snprintf(buf, sizeof(buf),
                "%s  %s\n"
                "[LEFT] Back\n"
                "[RIGHT] View  [DOWN] Delete",
                e.name, sz);
        } else if (previewable) {
            snprintf(buf, sizeof(buf),
                "Name: %s\n"
                "Size: %s\n"
                "\n"
                "[LEFT] Back\n"
                "[RIGHT] View  [DOWN] Delete",
                e.name, sz);
        } else {
            snprintf(buf, sizeof(buf),
                "Name: %s\n"
                "Size: %s\n"
                "\n"
                "[LEFT] Back\n"
                "[RIGHT] Delete",
                e.name, sz);
        }
    }
    lv_label_set_text(info_label_, buf);
}

void FilesApp::complete_preview()
{
    if (thumb_.valid()) {
        int area_h = LCD_V_RES - CONTENT_Y;
        int thumb_x = (LCD_H_RES - thumb_.width) / 2;
        int thumb_y = CONTENT_Y + (area_h - thumb_.height) / 2;
        thumb_canvas_ = lv_canvas_create(screen_);
        lv_canvas_set_buffer(thumb_canvas_, thumb_.pixels,
                             thumb_.width, thumb_.height,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(thumb_canvas_, thumb_x, thumb_y);
    } else {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
        lv_label_set_text(info_label_, "Cannot decode image");
    }
}

void FilesApp::complete_text_preview()
{
    auto &e = entries_[selected_idx_];
    char full[256];
    join_path(full, sizeof(full), path_, e.name);

    text_buf_ = static_cast<char *>(
        heap_caps_malloc(TEXT_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!text_buf_) {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
        lv_label_set_text(info_label_, "Out of memory");
        return;
    }

    FILE *f = fopen(full, "rb");
    size_t nread = 0;
    if (f) {
        nread = fread(text_buf_, 1, TEXT_BUF_SIZE - 1, f);
        fclose(f);
    }
    text_buf_[nread] = '\0';

    for (size_t i = 0; i < nread; i++) {
        unsigned char c = text_buf_[i];
        if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) {
            text_buf_[i] = '.';
        }
    }

    int area_h = LCD_V_RES - CONTENT_Y;

    text_container_ = lv_obj_create(screen_);
    lv_obj_set_size(text_container_, LCD_H_RES, area_h);
    lv_obj_set_pos(text_container_, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(text_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_container_, 0, 0);
    lv_obj_set_style_pad_left(text_container_, PAD, 0);
    lv_obj_set_style_pad_right(text_container_, PAD, 0);
    lv_obj_set_style_pad_top(text_container_, 0, 0);
    lv_obj_set_scrollbar_mode(text_container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(text_container_, LV_DIR_VER);

    lv_obj_t *lbl = lv_label_create(text_container_);
    lv_obj_set_style_text_font(lbl, font_mono(), 0);
    lv_obj_set_style_text_color(lbl, color::fg(), 0);
    lv_obj_set_width(lbl, LCD_H_RES - 2 * PAD);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, text_buf_);
}

// ---------------------------------------------------------------------------
// Directory scanning
// ---------------------------------------------------------------------------

void FilesApp::on_enter(lv_obj_t *screen)
{
    screen_ = screen;
    page_ = Page::BROWSE;
    depth_ = 0;
    selected_idx_ = 0;
    std::strncpy(path_, SD_MOUNT, sizeof(path_));
    scan_dir();
    build_page();
}

void FilesApp::scan_dir()
{
    entry_count_ = 0;

    DIR *dir = opendir(path_);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open: %s", path_);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) && entry_count_ < MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;

        auto &e = entries_[entry_count_];
        std::strncpy(e.name, ent->d_name, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        e.is_dir = (ent->d_type == DT_DIR);
        e.size = 0;

        if (!e.is_dir) {
            char full[256];
            join_path(full, sizeof(full), path_, e.name);
            struct stat st;
            if (stat(full, &st) == 0) {
                e.size = st.st_size;
            }
        }

        entry_count_++;
    }
    closedir(dir);

    std::sort(entries_, entries_ + entry_count_, [](const Entry &a, const Entry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return strcasecmp(a.name, b.name) < 0;
    });
}

// ---------------------------------------------------------------------------
// Page building
// ---------------------------------------------------------------------------

void FilesApp::build_page()
{
    menu_.destroy();
    if (spinner_) { lv_obj_delete(spinner_); spinner_ = nullptr; }
    if (info_label_) {
        lv_obj_delete(info_label_);
        info_label_ = nullptr;
    }
    free_preview();

    switch (page_) {
    case Page::BROWSE: {
        if (!sd_->mounted()) {
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_mono(), 0);
            lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
            lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
            lv_label_set_text(info_label_, "No SD card");
            break;
        }

        static const char *labels[MAX_ENTRIES + 1];
        static char label_buf[MAX_ENTRIES + 1][40];
        int menu_count = 0;

        for (int i = 0; i < entry_count_; i++) {
            if (entries_[i].is_dir) {
                snprintf(label_buf[menu_count], sizeof(label_buf[menu_count]),
                         "/%.38s", entries_[i].name);
            } else {
                char sz[16];
                format_size(sz, sizeof(sz), entries_[i].size);
                snprintf(label_buf[menu_count], sizeof(label_buf[menu_count]),
                         "%.22s %s", entries_[i].name, sz);
            }
            labels[menu_count] = label_buf[menu_count];
            menu_count++;
        }

        if (depth_ == 0) {
            if (server_->running()) {
                snprintf(label_buf[menu_count], sizeof(label_buf[menu_count]),
                         "[Server: ON]");
            } else {
                snprintf(label_buf[menu_count], sizeof(label_buf[menu_count]),
                         "[File Server]");
            }
            labels[menu_count] = label_buf[menu_count];
            menu_count++;
        }

        if (menu_count == 0) {
            info_label_ = lv_label_create(screen_);
            lv_obj_set_style_text_font(info_label_, font_mono(), 0);
            lv_obj_set_style_text_color(info_label_, color::fg_dim(), 0);
            lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
            lv_label_set_text(info_label_, "Empty folder");
            break;
        }

        menu_.create(screen_, CONTENT_Y, labels, menu_count);
        menu_.set_cursor(selected_idx_);
        break;
    }

    case Page::FILE_INFO: {
        auto &e = entries_[selected_idx_];
        FileKind kind = e.is_dir ? FileKind::OTHER : file_kind(e.name);

        if (kind == FileKind::IMAGE) {
            show_loading();
            lv_refr_now(NULL);
            char full[256];
            join_path(full, sizeof(full), path_, e.name);
            loader_.start(full, 200, 100);
            break;
        }

        complete_file_info();
        break;
    }

    case Page::CONFIRM_DELETE: {
        int idx = selected_idx_;
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        char buf[128];
        snprintf(buf, sizeof(buf),
            "Delete %s?\n"
            "\n"
            "[LEFT] Cancel\n"
            "[RIGHT] Confirm",
            entries_[idx].name);
        lv_label_set_text(info_label_, buf);
        break;
    }

    case Page::PREVIEW: {
        auto &e = entries_[selected_idx_];
        FileKind kind = file_kind(e.name);

        if (kind == FileKind::IMAGE) {
            show_loading();
            lv_refr_now(NULL);
            char full[256];
            join_path(full, sizeof(full), path_, e.name);
            loader_.start(full, LCD_H_RES, LCD_V_RES - CONTENT_Y);
        } else if (kind == FileKind::TEXT) {
            complete_text_preview();
        }
        break;
    }

    case Page::EJECT: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);
        lv_label_set_text(info_label_,
            "SD card safely ejected.\n"
            "You can remove it now.\n"
            "\n"
            "[LEFT] Back");
        break;
    }

    case Page::SERVER: {
        info_label_ = lv_label_create(screen_);
        lv_obj_set_style_text_font(info_label_, font_mono(), 0);
        lv_obj_set_style_text_color(info_label_, color::fg(), 0);
        lv_obj_set_pos(info_label_, PAD, CONTENT_Y);

        if (wifi_->state() != WifiState::CONNECTED) {
            lv_label_set_text(info_label_,
                "WiFi not connected.\n"
                "Connect to WiFi first.\n"
                "\n"
                "[LEFT] Back");
        } else if (server_->running()) {
            char ip[16];
            wifi_->get_ip(ip, sizeof(ip));
            char buf[160];
            snprintf(buf, sizeof(buf),
                "Server running\n"
                "http://os32.local:8080\n"
                "http://%.15s:8080\n"
                "\n"
                "[LEFT] Back\n"
                "[RIGHT] Stop",
                ip);
            lv_label_set_text(info_label_, buf);
        } else {
            lv_label_set_text(info_label_,
                "File Server\n"
                "Share SD over WiFi\n"
                "\n"
                "[LEFT] Back\n"
                "[RIGHT] Start");
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool FilesApp::navigate_into()
{
    int idx = menu_.cursor();
    if (idx < 0 || idx >= entry_count_) return false;
    if (!entries_[idx].is_dir) return false;

    if (depth_ < MAX_DEPTH) {
        cursor_stack_[depth_] = idx;
        depth_++;
    }

    char new_path[256];
    join_path(new_path, sizeof(new_path), path_, entries_[idx].name);
    std::strncpy(path_, new_path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    selected_idx_ = 0;
    scan_dir();
    page_ = Page::BROWSE;
    build_page();
    return true;
}

void FilesApp::navigate_up()
{
    if (strcmp(path_, SD_MOUNT) == 0) return;

    char *last_slash = strrchr(path_, '/');
    if (last_slash && last_slash > path_) {
        *last_slash = '\0';
        if (strlen(path_) < strlen(SD_MOUNT)) {
            std::strncpy(path_, SD_MOUNT, sizeof(path_));
        }
    }

    if (depth_ > 0) {
        depth_--;
        selected_idx_ = cursor_stack_[depth_];
    } else {
        selected_idx_ = 0;
    }
    scan_dir();
    page_ = Page::BROWSE;
    build_page();
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void FilesApp::on_update(uint32_t)
{
    if (server_->running() && wifi_->state() != WifiState::CONNECTED) {
        server_->stop();
        if (page_ == Page::SERVER) {
            build_page();
        }
    }

    if (page_ == Page::BROWSE && sd_->consume_change()) {
        int cursor = menu_.cursor();
        scan_dir();
        int total = entry_count_ + (depth_ == 0 ? 1 : 0);
        if (cursor >= total && total > 0) {
            cursor = total - 1;
        }
        selected_idx_ = (cursor < entry_count_) ? cursor : 0;
        build_page();
    }

    auto ls = loader_.state();
    if (ls == ThumbLoader::State::DONE || ls == ThumbLoader::State::FAILED) {
        if (page_ == Page::FILE_INFO || page_ == Page::PREVIEW) {
            if (spinner_) { lv_obj_delete(spinner_); spinner_ = nullptr; }
            if (info_label_) { lv_obj_delete(info_label_); info_label_ = nullptr; }

            if (ls == ThumbLoader::State::DONE) {
                thumb_ = loader_.take_result();
            } else {
                Thumbnail t = loader_.take_result();
                t.release();
            }

            if (page_ == Page::FILE_INFO) complete_file_info();
            else complete_preview();
        } else {
            Thumbnail t = loader_.take_result();
            t.release();
        }
    }
}

void FilesApp::on_exit()
{
    loader_.cancel();
    while (loader_.state() == ThumbLoader::State::LOADING)
        vTaskDelay(pdMS_TO_TICKS(5));

    if (loader_.state() == ThumbLoader::State::DONE ||
        loader_.state() == ThumbLoader::State::FAILED) {
        Thumbnail t = loader_.take_result();
        t.release();
    }

    menu_.invalidate();
    info_label_ = nullptr;
    spinner_ = nullptr;
    thumb_canvas_ = nullptr;
    text_container_ = nullptr;
    thumb_.release();
    if (text_buf_) { heap_caps_free(text_buf_); text_buf_ = nullptr; }
    screen_ = nullptr;
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------

bool FilesApp::on_button(ButtonId btn, ButtonEvent event)
{
    if (event == ButtonEvent::RELEASE) return false;

    if (page_ == Page::EJECT) {
        if (btn == ButtonId::LEFT) return false;
        return true;
    }

    if (page_ == Page::SERVER) {
        if (btn == ButtonId::LEFT) {
            page_ = Page::BROWSE;
            build_page();
        } else if (btn == ButtonId::RIGHT && event == ButtonEvent::PRESS) {
            if (wifi_->state() == WifiState::CONNECTED) {
                if (server_->running()) server_->stop();
                else server_->start(sd_);
                build_page();
            }
        }
        return true;
    }

    if (page_ == Page::CONFIRM_DELETE) {
        if (btn == ButtonId::RIGHT && event == ButtonEvent::PRESS) {
            char full[256];
            join_path(full, sizeof(full), path_, entries_[selected_idx_].name);

            if (entries_[selected_idx_].is_dir) rmdir(full);
            else unlink(full);
            ESP_LOGI(TAG, "Deleted: %s", full);

            scan_dir();
            if (selected_idx_ >= entry_count_ && entry_count_ > 0) {
                selected_idx_ = entry_count_ - 1;
            }
            page_ = Page::BROWSE;
            build_page();
        } else if (btn == ButtonId::LEFT) {
            page_ = Page::FILE_INFO;
            build_page();
        }
        return true;
    }

    if (page_ == Page::PREVIEW) {
        if (btn == ButtonId::LEFT) {
            page_ = Page::FILE_INFO;
            build_page();
        } else if (text_container_ && (btn == ButtonId::UP || btn == ButtonId::DOWN)) {
            int step = (FONT_H + 2) * 3;
            if (btn == ButtonId::UP) {
                int avail = lv_obj_get_scroll_top(text_container_);
                if (avail > 0)
                    lv_obj_scroll_by(text_container_, 0, LV_MIN(step, avail), LV_ANIM_OFF);
            } else {
                int avail = lv_obj_get_scroll_bottom(text_container_);
                if (avail > 0)
                    lv_obj_scroll_by(text_container_, 0, -LV_MIN(step, avail), LV_ANIM_OFF);
            }
        }
        return true;
    }

    if (page_ == Page::FILE_INFO) {
        auto &e = entries_[selected_idx_];
        FileKind kind = e.is_dir ? FileKind::OTHER : file_kind(e.name);
        bool previewable = (kind == FileKind::IMAGE || kind == FileKind::TEXT);

        if (btn == ButtonId::LEFT) {
            page_ = Page::BROWSE;
            build_page();
        } else if (btn == ButtonId::RIGHT && event == ButtonEvent::PRESS) {
            if (previewable) {
                page_ = Page::PREVIEW;
                build_page();
            } else {
                page_ = Page::CONFIRM_DELETE;
                build_page();
            }
        } else if (btn == ButtonId::DOWN && event == ButtonEvent::PRESS && previewable) {
            page_ = Page::CONFIRM_DELETE;
            build_page();
        }
        return true;
    }

    // Page::BROWSE
    switch (btn) {
    case ButtonId::UP:
        menu_.move_up();
        return true;
    case ButtonId::DOWN:
        menu_.move_down();
        return true;
    case ButtonId::RIGHT:
        if (event == ButtonEvent::PRESS) {
            int cursor = menu_.cursor();

            if (depth_ == 0 && cursor == entry_count_) {
                selected_idx_ = cursor;
                page_ = Page::SERVER;
                build_page();
                return true;
            }

            if (cursor >= 0 && cursor < entry_count_) {
                selected_idx_ = cursor;
                if (entries_[selected_idx_].is_dir) {
                    navigate_into();
                } else {
                    page_ = Page::FILE_INFO;
                    build_page();
                }
            }
        }
        return true;
    case ButtonId::LEFT:
        if (strcmp(path_, SD_MOUNT) == 0) {
            return false;
        }
        navigate_up();
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Header / status
// ---------------------------------------------------------------------------

void FilesApp::get_header_text(char *buf, std::size_t len) const
{
    if (len == 0) return;

    const char *rel = path_ + strlen(SD_MOUNT);
    if (rel[0] == '\0') rel = "/";

    if (page_ == Page::PREVIEW) {
        snprintf(buf, len, "Files %s > View", rel);
    } else if (page_ == Page::FILE_INFO || page_ == Page::CONFIRM_DELETE) {
        snprintf(buf, len, "Files %s > Info", rel);
    } else if (page_ == Page::EJECT) {
        snprintf(buf, len, "Files > Ejected");
    } else if (page_ == Page::SERVER) {
        snprintf(buf, len, "Files > Server");
    } else {
        snprintf(buf, len, "Files %s", rel);
    }
}

void FilesApp::get_status_text(char *line1, char *line2)
{
    if (page_ == Page::SERVER) {
        snprintf(line1, 17, "File Server");
        if (server_->running()) {
            char ip[16];
            wifi_->get_ip(ip, sizeof(ip));
            snprintf(line2, 17, "%.15s", ip);
        } else {
            snprintf(line2, 17, "Stopped");
        }
        return;
    }

    const char *rel = path_ + strlen(SD_MOUNT);
    if (rel[0] == '\0') rel = "/";

    snprintf(line1, 17, "Files %-10s", rel);
    if (entry_count_ > 0 && page_ == Page::BROWSE && menu_.cursor() < entry_count_) {
        snprintf(line2, 17, "> %.14s", entries_[menu_.cursor()].name);
    } else {
        snprintf(line2, 17, "%d items", entry_count_ & 0xFF);
    }
}

} // namespace os32
