#pragma once

#include "app.h"

namespace os32 {

class SysMonApp : public App {
public:
    const char* name() const override { return "System Monitor"; }
    void on_enter(lv_obj_t *screen) override;
    void on_update(uint32_t dt_ms) override;
    void on_exit() override;
    bool on_button(ButtonId btn, ButtonEvent event) override;
    void get_status_text(char *line1, char *line2) override;

private:
    lv_obj_t *info_label_ = nullptr;
    uint32_t refresh_accum_ = 0;
    uint32_t status_accum_ = 0;
    int status_page_ = 0;
};

} // namespace os32
