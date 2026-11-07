#include "board_i2c.h"

#include <stdint.h>

#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";

static i2c_master_bus_handle_t bus;

esp_err_t board_i2c_init(void)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,  // the C6 has a single HP I2C controller
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &bus);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return bus;
}

esp_err_t board_i2c_scan(uint8_t *found)
{
    *found = 0;
    for (uint16_t addr = 0x08; addr < 0x78; addr++)
    {
        esp_err_t err = i2c_master_probe(bus, addr, BOARD_I2C_TIMEOUT_MS);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Device at 0x%02X", addr);
            (*found)++;
        }
        else if (err != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Probe 0x%02X: %s", addr, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
