#include <stdbool.h>

#include "board_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

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
}
