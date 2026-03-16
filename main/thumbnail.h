#pragma once

#include <cstdint>

namespace os32 {

struct Thumbnail {
    uint16_t *pixels = nullptr;   // RGB565, PSRAM-allocated
    int width = 0;
    int height = 0;

    bool valid() const { return pixels != nullptr; }
    void release();
};

// Decode image and scale to fit within max_w × max_h, preserving aspect ratio.
// Supports .bmp (24-bit), .jpg/.jpeg.  Returns invalid thumbnail on failure.
Thumbnail thumbnail_load(const char *path, int max_w, int max_h);

// Runs thumbnail_load asynchronously on core 1.
// Call start() to begin, poll state() from the UI thread, take_result() when DONE.
class ThumbLoader {
public:
    enum class State : uint8_t { IDLE, LOADING, DONE, FAILED };

    void start(const char *path, int max_w, int max_h);
    void cancel();
    State state() const { return state_; }
    Thumbnail take_result();

private:
    static void task_func(void *arg);

    volatile State state_ = State::IDLE;
    volatile bool cancel_ = false;
    char path_[256] = {};
    int max_w_ = 0;
    int max_h_ = 0;
    Thumbnail result_ = {};
};

} // namespace os32
