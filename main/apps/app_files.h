#pragma once

#include "app.h"
#include "menu.h"
#include "sd_manager.h"
#include "thumbnail.h"
#include <dirent.h>

namespace os32 {

class WifiManager;
class FileServer;

class FilesApp : public App {
public:
    FilesApp(SdManager *sd, WifiManager *wifi, FileServer *server)
        : sd_(sd), wifi_(wifi), server_(server) {}
    const char* name() const override { return "Files"; }
    void on_enter(lv_obj_t *screen) override;
    void on_update(uint32_t dt_ms) override;
    void on_exit() override;
    bool on_button(ButtonId btn, ButtonEvent event) override;
    void get_status_text(char *line1, char *line2) override;
    void get_header_text(char *buf, std::size_t len) const override;

private:
    static constexpr int MAX_ENTRIES = 64;
    static constexpr int TEXT_BUF_SIZE = 4096;

    enum class Page { BROWSE, FILE_INFO, CONFIRM_DELETE, PREVIEW, EJECT, SERVER };

    enum class FileKind { OTHER, IMAGE, TEXT };

    struct Entry {
        char name[32];
        bool is_dir;
        uint32_t size;
    };

    static FileKind file_kind(const char *name);
    void scan_dir();
    void build_page();
    void show_loading();
    void complete_file_info();
    void complete_preview();
    void complete_text_preview();
    bool navigate_into();
    void navigate_up();
    void free_preview();

    SdManager *sd_;
    WifiManager *wifi_;
    FileServer *server_;
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *info_label_ = nullptr;
    lv_obj_t *spinner_ = nullptr;
    lv_obj_t *thumb_canvas_ = nullptr;
    lv_obj_t *text_container_ = nullptr;
    ThumbLoader loader_;
    Thumbnail thumb_;
    char *text_buf_ = nullptr;
    Menu menu_;
    Page page_ = Page::BROWSE;

    static constexpr int MAX_DEPTH = 8;

    char path_[256];
    Entry entries_[MAX_ENTRIES];
    int entry_count_ = 0;
    int selected_idx_ = 0;
    int cursor_stack_[MAX_DEPTH];
    int depth_ = 0;
};

} // namespace os32
