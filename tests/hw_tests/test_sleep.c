// M2: the wake reason as an LED colour, then sleep with a 30 s RTC wake.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_SLEEP

#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "power.h"
#include "st75256.h"

static const char *TAG = "hw_test";

// NULL on failure.
static led_strip_handle_t led_init(void)
{
    led_strip_handle_t       led = NULL;
    const led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED,
        .max_leds = 1,
        .color_component_format = PIN_LED_COLOR_FORMAT,
    };
    const led_strip_rmt_config_t rmt_config = {.resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &led) != ESP_OK)
    {
        ESP_LOGE(TAG, "LED init failed");
        return NULL;
    }
    return led;
}

// Wake reason as an LED colour for 10 s: COLD_BOOT white, BUTTON green,
// TIMER blue, OTHER red. Then sleeps with a 30 s RTC wake.
void test_sleep_run(void)
{
    static const uint8_t colours[][3] = {
        [POWER_WAKE_COLD_BOOT] = {16, 16, 16},
        [POWER_WAKE_BUTTON] = {0, 16, 0},
        [POWER_WAKE_TIMER] = {0, 0, 16},
        [POWER_WAKE_OTHER] = {16, 0, 0},
    };
    const uint8_t *c = colours[power_wake_reason()];

    led_strip_handle_t led = led_init();
    for (int i = 0; i < 10; i++)
    {
        if (led)
        {
            led_strip_set_pixel(led, 0, c[0], c[1], c[2]);
            led_strip_refresh(led);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (led)
        {
            led_strip_clear(led);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    st75256_sleep();
    power_sleep(30);
}

#endif
