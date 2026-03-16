#pragma once

#include <cstdint>

namespace os32 {

enum class IdleState : uint8_t { ACTIVE, DIM, SLEEP };

class IdleTimer {
public:
    void init();                    // Load timeout from NVS
    void reset();                   // Reset on user input
    void update(uint32_t dt_ms);    // Accumulate idle time
    IdleState state() const { return state_; }

    uint16_t timeout_sec() const { return timeout_sec_; }
    void set_timeout(uint16_t sec); // Set and persist to NVS (0 = disabled)

    static constexpr uint16_t TIMEOUT_OPTIONS[] = { 0, 30, 60, 120, 300, 600 };
    static constexpr int TIMEOUT_COUNT = 6;
    static const char* timeout_label(uint16_t sec);

private:
    IdleState state_ = IdleState::ACTIVE;
    uint32_t idle_ms_ = 0;
    uint16_t timeout_sec_ = 0;     // 0 = disabled
};

} // namespace os32
