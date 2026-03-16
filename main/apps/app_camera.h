#pragma once

#include "app.h"

namespace os32 {

class SdManager;

class CameraApp : public App {
public:
    CameraApp(SdManager *sd) : sd_(sd) {}
    const char* name() const override { return "Camera"; }
    void on_enter(lv_obj_t *screen) override;
    void on_update(uint32_t dt_ms) override;
    void on_exit() override;
    bool on_button(ButtonId btn, ButtonEvent event) override;
    void get_status_text(char *line1, char *line2) override;
    bool uses_direct_render() const override { return true; }

private:
    bool init_camera();
    void push_frame();
    void save_snapshot();
    void apply_filter();
    void scan_existing_snapshots();

    SdManager *sd_;
    bool cam_ready_ = false;
    int snap_next_ = 0;    // Next filename index (highest existing + 1)
    int snap_count_ = 0;   // Actual number of files in DCIM
    uint32_t snap_msg_timer_ = 0;
    int filter_ = 0;
    static constexpr int FILTER_COUNT = 7;
};

} // namespace os32
