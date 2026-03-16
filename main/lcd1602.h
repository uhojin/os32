#pragma once

#include "driver/i2c_master.h"
#include <cstdint>

namespace os32 {

class LCD1602 {
public:
    bool init(int sda_pin, int scl_pin, uint8_t addr = 0x27);
    void clear();
    void set_cursor(int col, int row);
    void print(const char *str);
    void set_line(int row, const char *text);
    void set_backlight(bool on);

private:
    void send_nibble(uint8_t nibble, uint8_t mode);
    void send_byte(uint8_t byte, uint8_t mode);
    void write_cmd(uint8_t cmd);
    void write_data(uint8_t data);
    void pulse_enable(uint8_t data);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t backlight_ = 0x08;  // backlight bit
};

} // namespace os32
