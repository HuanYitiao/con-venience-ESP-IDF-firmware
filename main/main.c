#include <stdbool.h>
#include <stdio.h>

#include "battery.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "board_spi.h"
#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mcp23017.h"
#include "nvs_flash.h"
#include "pcf85063a.h"
#include "power.h"
#include "st75256.h"

static const char *TAG = "con-venience";

// M2 sleep/wake test, see sleep_test(). Off by default: while it runs the
// board is switched off most of the time, so reflashing needs a wake first.
#define M2_SLEEP_TEST 0

// M5 display tuning, see display_tune(). Replaces blink_forever().
#define M5_DISPLAY_TUNE 0

typedef struct
{
    button_id_t     id;
    button_action_t action;
} button_event_t;

// Set while display_tune() runs; button events are forwarded to it.
static QueueHandle_t tune_queue;

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

// M3 test: every click and long press shows up in the log.
static void on_button(button_id_t id, button_action_t action)
{
    static const char *const names[BUTTON_COUNT] = {
        [BUTTON_UP] = "UP",       [BUTTON_DOWN] = "DOWN", [BUTTON_LEFT] = "LEFT",
        [BUTTON_RIGHT] = "RIGHT", [BUTTON_PAIR] = "PAIR",
    };
    ESP_LOGI(TAG, "Button %s %s", names[id], action == BUTTON_LONG_PRESS ? "long press" : "click");

    if (tune_queue)
    {
        const button_event_t ev = {.id = id, .action = action};
        xQueueSend(tune_queue, &ev, 0);
    }
}

#if M2_SLEEP_TEST
// Shows the wake reason as an LED colour for 10 s, then sleeps with a 30 s
// RTC wake: COLD_BOOT white, BUTTON green, TIMER blue, OTHER red.
static void sleep_test(void)
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
#else
// M1 acceptance test: on battery only, the LED must keep blinking past ~6 s
// (U1 hold time). Remove once a real UI exists.
static void blink_forever(void)
{
    led_strip_handle_t led = led_init();
    if (!led)
    {
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
#endif

// Standard IDF init: a full or outdated NVS partition is erased and set up again.
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            err = nvs_flash_init();
        }
    }
    return err;
}

// M4 test: VBAT against a multimeter, and the gauge. The level must survive
// a sleep and be forgotten after a battery reinsert.
static void battery_test(void)
{
    esp_err_t err = battery_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Battery init failed: %s", esp_err_to_name(err));
        return;
    }

    // COLD_BOOT also covers esp_restart, panic and watchdog resets; only a
    // real power-up means the cells may have been swapped.
    bool powered_up = power_wake_reason() == POWER_WAKE_COLD_BOOT
                      && esp_reset_reason() == ESP_RST_POWERON;
    battery_level_init(powered_up);

    for (int i = 0; i < 3; i++)
    {
        uint16_t mv = 0;
        err = battery_read_mv(&mv);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "VBAT read failed: %s", esp_err_to_name(err));
            return;
        }
        battery_level_update(mv);
        ESP_LOGI(TAG, "VBAT %u mV, level %u/%u", mv, battery_level(), BATTERY_LEVELS);
    }
}

static void set_pixel(uint8_t *frame, int x, int y, uint8_t gray)
{
    uint8_t *byte = &frame[(y / ST75256_PAGE_HEIGHT) * ST75256_WIDTH + x];
    int      shift = (ST75256_PAGE_HEIGHT - 1 - y % ST75256_PAGE_HEIGHT) * 2;
    *byte = (*byte & ~(0x3 << shift)) | (gray << shift);
}

// M5 test pattern, in display coordinates (x 0..255, y 0..127):
//   x   0..127, y  0..63: four 32-column bands, white, light, dark, black.
//   x 128..255, y  0..63: a 32x32 black square on white. Crosstalk shows up
//                         as shadows in the same rows or columns.
//   x   0..127, y 64..127: 1-pixel horizontal stripes, white, light, dark,
//                         black from the top.
//   x 128..255, y 64..127: a 1-pixel black diagonal on white. Straight if the
//                         pixel order within a byte is right, a zigzag if not.
// Returns false if the display could not be set up.
static bool display_test(void)
{
    static uint8_t frame[ST75256_PAGES * ST75256_WIDTH];
    for (int y = 0; y < ST75256_HEIGHT; y++)
    {
        for (int x = 0; x < ST75256_WIDTH; x++)
        {
            uint8_t gray = ST75256_WHITE;
            if (y < 64 && x < 128)
            {
                gray = x / 32;
            }
            else if (y < 64)
            {
                gray = (x >= 176 && x < 208 && y >= 16 && y < 48) ? ST75256_BLACK : ST75256_WHITE;
            }
            else if (x < 128)
            {
                gray = y % 4;
            }
            else
            {
                gray = (x - 128 == y - 64) ? ST75256_BLACK : ST75256_WHITE;
            }
            set_pixel(frame, x, y, gray);
        }
    }

    esp_err_t err = power_periph_on();  // backlight and SD rail, before the SPI bus
    if (err == ESP_OK)
    {
        err = board_spi_init();
    }
    if (err == ESP_OK)
    {
        err = st75256_init();
    }
    if (err == ESP_OK)
    {
        err = st75256_set_backlight(100);
    }
    if (err == ESP_OK)
    {
        err = st75256_write(0, 0, ST75256_WIDTH, ST75256_PAGES, frame);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Display test failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Display test pattern sent");
    return true;
}

#if M5_DISPLAY_TUNE && !M2_SLEEP_TEST
typedef enum
{
    TUNE_VOP,
    TUNE_BIAS,
    TUNE_BOOSTER,
    TUNE_LINE_INV,
    TUNE_FRAME_RATE,
    TUNE_COUNT,
} tune_param_t;

static const char *const tune_names[TUNE_COUNT] = {"vop", "bias", "booster", "line_inv",
                                                   "frame_rate"};
// frame_rate -1 = not sent (chip default).
static const int tune_min[TUNE_COUNT] = {0, 0, 0, 0, -1};
static const int tune_max[TUNE_COUNT] = {511, 5, 3, 255, 31};

static int tune_get(const st75256_tuning_t *t, tune_param_t p)
{
    switch (p)
    {
        case TUNE_VOP:
            return t->vop;
        case TUNE_BIAS:
            return t->bias;
        case TUNE_BOOSTER:
            return t->booster;
        case TUNE_LINE_INV:
            return t->line_inv;
        case TUNE_FRAME_RATE:
        default:
            return t->frame_rate == ST75256_FRAME_RATE_DEFAULT ? -1 : t->frame_rate;
    }
}

static void tune_set(st75256_tuning_t *t, tune_param_t p, int v)
{
    if (v < tune_min[p])
    {
        v = tune_min[p];
    }
    if (v > tune_max[p])
    {
        v = tune_max[p];
    }
    switch (p)
    {
        case TUNE_VOP:
            t->vop = v;
            break;
        case TUNE_BIAS:
            t->bias = v;
            break;
        case TUNE_BOOSTER:
            t->booster = v;
            break;
        case TUNE_LINE_INV:
            t->line_inv = v;
            break;
        case TUNE_FRAME_RATE:
        default:
            t->frame_rate = v < 0 ? ST75256_FRAME_RATE_DEFAULT : v;
            break;
    }
}

static void tune_log(const st75256_tuning_t *t, tune_param_t sel)
{
    char fr[12];
    if (t->frame_rate == ST75256_FRAME_RATE_DEFAULT)
    {
        snprintf(fr, sizeof(fr), "default");
    }
    else
    {
        snprintf(fr, sizeof(fr), "%u", t->frame_rate);
    }
    ESP_LOGI(TAG, "vop=%u bias=%u booster=%u line_inv=0x%02X frame_rate=%s  [editing %s]", t->vop,
             t->bias, t->booster, t->line_inv, fr, tune_names[sel]);
}

// LEFT/RIGHT pick a setting, UP/DOWN change it (long press: by 10), PAIR
// click logs the values as C, PAIR long press restores the defaults.
static void display_tune(void)
{
    tune_queue = xQueueCreate(8, sizeof(button_event_t));
    if (!tune_queue)
    {
        ESP_LOGE(TAG, "Tune queue failed");
        return;
    }

    const st75256_tuning_t defaults = st75256_get_tuning();
    st75256_tuning_t       t = defaults;
    tune_param_t           sel = TUNE_VOP;
    ESP_LOGI(TAG, "Display tuning: LEFT/RIGHT select, UP/DOWN change (long: x10), "
                  "PAIR print, PAIR long reset");
    tune_log(&t, sel);

    while (true)
    {
        button_event_t ev;
        xQueueReceive(tune_queue, &ev, portMAX_DELAY);
        int  step = ev.action == BUTTON_LONG_PRESS ? 10 : 1;
        bool changed = false;

        switch (ev.id)
        {
            case BUTTON_LEFT:
                sel = (sel + TUNE_COUNT - 1) % TUNE_COUNT;
                break;
            case BUTTON_RIGHT:
                sel = (sel + 1) % TUNE_COUNT;
                break;
            case BUTTON_UP:
                tune_set(&t, sel, tune_get(&t, sel) + step);
                changed = true;
                break;
            case BUTTON_DOWN:
                tune_set(&t, sel, tune_get(&t, sel) - step);
                changed = true;
                break;
            case BUTTON_PAIR:
            default:
                if (ev.action == BUTTON_LONG_PRESS)
                {
                    t = defaults;
                    changed = true;
                }
                else
                {
                    char fr[32];
                    if (t.frame_rate == ST75256_FRAME_RATE_DEFAULT)
                    {
                        snprintf(fr, sizeof(fr), "ST75256_FRAME_RATE_DEFAULT");
                    }
                    else
                    {
                        snprintf(fr, sizeof(fr), "%u", t.frame_rate);
                    }
                    ESP_LOGI(TAG, "C: {.vop = %u, .bias = %u, .booster = %u, .line_inv = 0x%02X, "
                                  ".frame_rate = %s}",
                             t.vop, t.bias, t.booster, t.line_inv, fr);
                }
                break;
        }

        if (changed)
        {
            esp_err_t err = st75256_set_tuning(&t);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Set tuning failed: %s", esp_err_to_name(err));
            }
        }
        tune_log(&t, sel);
    }
}
#endif

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

#if !M2_SLEEP_TEST
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
#endif

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
#if !M2_SLEEP_TEST
        // Takes 8 s; would delay the wake colour in the sleep test.
        timer_test();
#endif
    }
    else
    {
        ESP_LOGE(TAG, "PCF85063A init failed: %s", esp_err_to_name(rtc_err));
    }

    // After power_early_init (wake reason) and after timer_test, whose RTC
    // flag holds the shared INT line low for a few seconds.
    err = button_init(on_button);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(err));
    }

    err = nvs_init();
    if (err == ESP_OK)
    {
        battery_test();
    }
    else
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    bool display_ok = display_test();

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

#if M2_SLEEP_TEST
    (void)display_ok;
    sleep_test();
#elif M5_DISPLAY_TUNE
    if (display_ok)
    {
        display_tune();
    }
    blink_forever();
#else
    (void)display_ok;
    blink_forever();
#endif
}
