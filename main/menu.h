#pragma once

#include "os32.h"
#include "lvgl.h"

namespace os32 {

class Menu {
public:
    static constexpr int MAX_VISIBLE = 8;
    static constexpr int MAX_ITEMS = 72;

    void create(lv_obj_t *parent, int y_offset, const char *const *labels, int count);
    void destroy();
    void invalidate();

    bool move_up();
    bool move_down();

    int cursor() const { return cursor_; }
    void set_cursor(int c);
    int count() const { return total_count_; }
    bool valid() const { return total_count_ > 0; }

private:
    void update_styles();
    void refresh_labels();
    void ensure_visible();

    lv_obj_t *rows_[MAX_VISIBLE] = {};
    lv_obj_t *arrow_up_ = nullptr;
    lv_obj_t *arrow_down_ = nullptr;
    const char *labels_[MAX_ITEMS] = {};
    int total_count_ = 0;
    int visible_count_ = 0;
    int cursor_ = 0;
    int scroll_offset_ = 0;
};

} // namespace os32
