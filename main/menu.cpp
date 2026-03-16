#include "menu.h"

namespace os32 {

constexpr int MENU_ROW_H = FONT_H + 6;
constexpr int MENU_GAP = 2;

void Menu::create(lv_obj_t *parent, int y_offset, const char *const *labels, int count)
{
    destroy();

    total_count_ = (count > MAX_ITEMS) ? MAX_ITEMS : count;
    visible_count_ = (total_count_ > MAX_VISIBLE) ? MAX_VISIBLE : total_count_;
    cursor_ = 0;
    scroll_offset_ = 0;

    for (int i = 0; i < total_count_; i++) {
        labels_[i] = labels[i];
    }

    for (int i = 0; i < visible_count_; i++) {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, LCD_H_RES - 2 * PAD, MENU_ROW_H);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, PAD, y_offset + i * (MENU_ROW_H + MENU_GAP));
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, PAD, 0);
        lv_obj_set_style_pad_top(row, (MENU_ROW_H - FONT_H) / 2, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, labels_[i]);

        rows_[i] = row;
    }

    // Scroll arrows — appear at far right when items overflow
    if (total_count_ > visible_count_) {
        int arrow_x = LCD_H_RES - PAD - 12;
        int first_row_y = y_offset + (MENU_ROW_H - 14) / 2;
        int last_row_y = y_offset + (visible_count_ - 1) * (MENU_ROW_H + MENU_GAP)
                         + (MENU_ROW_H - 14) / 2;

        arrow_up_ = lv_label_create(parent);
        lv_obj_set_style_text_font(arrow_up_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow_up_, color::fg_dim(), 0);
        lv_obj_set_pos(arrow_up_, arrow_x, first_row_y);
        lv_label_set_text(arrow_up_, LV_SYMBOL_UP);

        arrow_down_ = lv_label_create(parent);
        lv_obj_set_style_text_font(arrow_down_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow_down_, color::fg_dim(), 0);
        lv_obj_set_pos(arrow_down_, arrow_x, last_row_y);
        lv_label_set_text(arrow_down_, LV_SYMBOL_DOWN);
    }

    update_styles();
}

void Menu::destroy()
{
    for (int i = 0; i < visible_count_; i++) {
        if (rows_[i]) {
            lv_obj_delete(rows_[i]);
            rows_[i] = nullptr;
        }
    }
    if (arrow_up_) { lv_obj_delete(arrow_up_); arrow_up_ = nullptr; }
    if (arrow_down_) { lv_obj_delete(arrow_down_); arrow_down_ = nullptr; }
    total_count_ = 0;
    visible_count_ = 0;
    cursor_ = 0;
    scroll_offset_ = 0;
}

void Menu::invalidate()
{
    for (int i = 0; i < visible_count_; i++) {
        rows_[i] = nullptr;
    }
    arrow_up_ = nullptr;
    arrow_down_ = nullptr;
    total_count_ = 0;
    visible_count_ = 0;
    cursor_ = 0;
    scroll_offset_ = 0;
}

void Menu::ensure_visible()
{
    if (cursor_ < scroll_offset_) {
        scroll_offset_ = cursor_;
    } else if (cursor_ >= scroll_offset_ + visible_count_) {
        scroll_offset_ = cursor_ - visible_count_ + 1;
    }
}

void Menu::refresh_labels()
{
    for (int i = 0; i < visible_count_; i++) {
        int data_idx = scroll_offset_ + i;
        lv_obj_t *label = lv_obj_get_child(rows_[i], 0);
        if (label && data_idx < total_count_) {
            lv_label_set_text(label, labels_[data_idx]);
        }
    }
}

void Menu::set_cursor(int c)
{
    if (total_count_ <= 0) return;
    cursor_ = (c >= 0 && c < total_count_) ? c : 0;
    int old_offset = scroll_offset_;
    ensure_visible();
    if (scroll_offset_ != old_offset) {
        refresh_labels();
    }
    update_styles();
}

bool Menu::move_up()
{
    if (total_count_ <= 0) return false;
    cursor_ = (cursor_ > 0) ? cursor_ - 1 : total_count_ - 1;
    int old_offset = scroll_offset_;
    ensure_visible();
    if (scroll_offset_ != old_offset) {
        refresh_labels();
    }
    update_styles();
    return true;
}

bool Menu::move_down()
{
    if (total_count_ <= 0) return false;
    cursor_ = (cursor_ < total_count_ - 1) ? cursor_ + 1 : 0;
    int old_offset = scroll_offset_;
    ensure_visible();
    if (scroll_offset_ != old_offset) {
        refresh_labels();
    }
    update_styles();
    return true;
}

void Menu::update_styles()
{
    for (int i = 0; i < visible_count_; i++) {
        lv_obj_t *row = rows_[i];
        bool selected = (scroll_offset_ + i == cursor_);

        if (selected) {
            lv_obj_set_style_bg_color(row, color::fg(), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        lv_obj_t *label = lv_obj_get_child(row, 0);
        if (label) {
            lv_obj_set_style_text_font(label, selected ? font_bold() : font_mono(), 0);
            lv_obj_set_style_text_color(label, selected ? color::bg() : color::fg_dim(), 0);
        }
    }

    if (arrow_up_)
        lv_obj_set_style_opa(arrow_up_, scroll_offset_ > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (arrow_down_)
        lv_obj_set_style_opa(arrow_down_,
            scroll_offset_ + visible_count_ < total_count_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

} // namespace os32
