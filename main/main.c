#include <stdbool.h>

#include "board_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mcp23017.h"

static const char *TAG = "con-venience";

void app_main(void)
{
    // No ESP_ERROR_CHECK here: an I2C error must not turn into a reboot loop.
    uint8_t   found = 0;
    esp_err_t err = board_i2c_init();
    if (err == ESP_OK)
    {
        err = board_i2c_scan(&found);
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
    }
    else
    {
        ESP_LOGE(TAG, "I2C failed: %s", esp_err_to_name(err));
    }

    err = mcp23017_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "MCP23017 init failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t iocon = 0;
    err = mcp23017_read_reg(MCP_IOCON, &iocon);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "MCP23017 read failed: %s", esp_err_to_name(err));
    }
    else if (iocon != 0x84)
    {
        ESP_LOGE(TAG, "MCP23017 IOCON is 0x%02X, expected 0x84", iocon);
    }
    else
    {
        ESP_LOGI(TAG, "MCP23017 IOCON OK: 0x%02X", iocon);
    }
}
