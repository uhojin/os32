#include "lcd1602.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "lcd1602";

// PCF8574 bit mapping for LCD
// D7 D6 D5 D4 BL EN RW RS
constexpr uint8_t LCD_RS = 0x01;
constexpr uint8_t LCD_EN = 0x04;
constexpr uint8_t LCD_BL = 0x08;

namespace os32 {

bool LCD1602::init(int sda_pin, int scl_pin, uint8_t addr)
{
    // Init I2C master bus
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Scan for device
    err = i2c_master_probe(bus_, addr, 100);
    if (err != ESP_OK) {
        // Try alternate address
        uint8_t alt = (addr == 0x27) ? 0x3F : 0x27;
        ESP_LOGW(TAG, "No device at 0x%02X, trying 0x%02X", addr, alt);
        err = i2c_master_probe(bus_, alt, 100);
        if (err == ESP_OK) {
            addr = alt;
        } else {
            ESP_LOGE(TAG, "No LCD1602 found on I2C bus");
            return false;
        }
    }
    ESP_LOGI(TAG, "Found LCD1602 at 0x%02X", addr);

    // Add device
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;

    err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %s", esp_err_to_name(err));
        return false;
    }

    // LCD init sequence (4-bit mode)
    vTaskDelay(pdMS_TO_TICKS(50));

    // Send 0x03 three times to reliably enter 8-bit mode first
    send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    send_nibble(0x03, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Switch to 4-bit mode
    send_nibble(0x02, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Function set: 4-bit, 2 lines, 5x8 font
    write_cmd(0x28);
    // Display on, cursor off, blink off
    write_cmd(0x0C);
    // Clear display
    write_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
    // Entry mode: increment, no shift
    write_cmd(0x06);

    ESP_LOGI(TAG, "LCD1602 initialized");
    return true;
}

void LCD1602::pulse_enable(uint8_t data)
{
    uint8_t on = data | LCD_EN;
    uint8_t off = data & ~LCD_EN;
    i2c_master_transmit(dev_, &on, 1, 100);
    i2c_master_transmit(dev_, &off, 1, 100);
}

void LCD1602::send_nibble(uint8_t nibble, uint8_t mode)
{
    uint8_t data = ((nibble & 0x0F) << 4) | mode | backlight_;
    pulse_enable(data);
}

void LCD1602::send_byte(uint8_t byte, uint8_t mode)
{
    send_nibble(byte >> 4, mode);
    send_nibble(byte & 0x0F, mode);
}

void LCD1602::write_cmd(uint8_t cmd)
{
    send_byte(cmd, 0);
}

void LCD1602::write_data(uint8_t data)
{
    send_byte(data, LCD_RS);
}

void LCD1602::clear()
{
    write_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
}

void LCD1602::set_cursor(int col, int row)
{
    uint8_t addr = col + (row == 0 ? 0x00 : 0x40);
    write_cmd(0x80 | addr);
}

void LCD1602::print(const char *str)
{
    while (*str) {
        write_data(static_cast<uint8_t>(*str));
        str++;
    }
}

void LCD1602::set_line(int row, const char *text)
{
    set_cursor(0, row);
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", text);
    print(buf);
}

void LCD1602::set_backlight(bool on)
{
    backlight_ = on ? LCD_BL : 0;
    uint8_t data = backlight_;
    i2c_master_transmit(dev_, &data, 1, 100);
}

} // namespace os32
