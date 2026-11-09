#include <stdbool.h>

#include "board_i2c.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mcp23017.h"
#include "pcf85063a.h"
#include "power.h"

static const char *TAG = "con-venience";

// M1 acceptance test: on battery only, the LED must keep blinking past ~6 s
// (U1 hold time). Remove once a real UI exists.
static void blink_forever(void)
{
    led_strip_handle_t           led = NULL;
    const led_strip_config_t     strip_config = {.strip_gpio_num = PIN_LED, .max_leds = 1};
    const led_strip_rmt_config_t rmt_config = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &led) != ESP_OK)
    {
        ESP_LOGE(TAG, "LED init failed");
        return;
    }

    bool on = false;
    while (true)
    {
        on = !on;
        if (on)
        {
            led_strip_set_pixel(led, 0, 16, 16, 16);
            led_strip_refresh(led);
        }
        else
        {
            led_strip_clear(led);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// M2 step 2 test: sets a fixed time once, later boots must find it trusted.
static void rtc_test(void)
{
    pcf85063a_time_t t = {0};
    bool             trusted = false;
    esp_err_t        err = pcf85063a_read_time(&t, &trusted);
    if (err == ESP_OK && !trusted)
    {
        ESP_LOGW(TAG, "RTC untrusted, setting it");
        const pcf85063a_time_t set = {
            .year = 2026, .month = 11, .day = 9, .hours = 12, .minutes = 0, .seconds = 0};
        err = pcf85063a_set_time(&set);
        if (err == ESP_OK)
        {
            err = pcf85063a_read_time(&t, &trusted);
        }
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RTC failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "RTC %04u-%02u-%02u %02u:%02u:%02u, %s", t.year, t.month, t.day, t.hours,
             t.minutes, t.seconds, trusted ? "trusted" : "untrusted");
}

// M2 step 3 test: a 5 s countdown must set TF and pull the shared INT line
// (GPIO5) low, and clearing the flag must release it again.
static void timer_test(void)
{
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_MCP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&int_cfg);
    if (err == ESP_OK)
    {
        err = pcf85063a_start_timer(5);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Timer start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Timer started, 5 s");

    for (int i = 1; i <= 8; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t flags = 0;
        err = pcf85063a_read_flags(&flags);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Read flags failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "t=%d s: TF=%d INT=%d", i, (flags & PCF85063A_FLAG_TF) ? 1 : 0,
                 gpio_get_level(PIN_MCP_INT));
    }

    err = pcf85063a_clear_flags();
    if (err == ESP_OK)
    {
        err = pcf85063a_stop_timer();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Timer cleanup failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Flags cleared, timer stopped: INT=%d", gpio_get_level(PIN_MCP_INT));
}

void app_main(void)
{
    // No ESP_ERROR_CHECK here: an I2C error must not turn into a reboot loop.
    // Keep-alive comes first; U1 only holds the MCU up for ~6 s.
    esp_err_t err = board_i2c_init();
    if (err == ESP_OK)
    {
        err = mcp23017_init();
    }
    // Before power_early_init, which reads the RTC flags. Safe this early:
    // init never clears a wake flag, so a waking INT keeps holding the power.
    esp_err_t rtc_err = pcf85063a_init();
    if (err == ESP_OK)
    {
        err = power_early_init();
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Keep-alive set, wake: %s", power_wake_name(power_wake_reason()));
    }
    else
    {
        ESP_LOGE(TAG, "Keep-alive failed: %s", esp_err_to_name(err));
    }

    if (rtc_err == ESP_OK)
    {
        rtc_test();
        timer_test();
    }
    else
    {
        ESP_LOGE(TAG, "PCF85063A init failed: %s", esp_err_to_name(rtc_err));
    }

    uint8_t found = 0;
    err = board_i2c_scan(&found);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "I2C scan done, %d device(s)", found);
    }
    else
    {
        ESP_LOGE(TAG, "I2C scan failed: %s", esp_err_to_name(err));
    }

    blink_forever();
}
