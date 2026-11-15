#include "hw_tests.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "max98357a.h"
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "power.h"
#include "sdcard.h"
#include "st75256.h"

#if HW_TEST != HW_TEST_NONE
static const char *TAG = "hw_test";
#endif

#if HW_TEST == HW_TEST_SLEEP
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
#endif

#if HW_TEST == HW_TEST_DISPLAY_TUNE
static void set_pixel(uint8_t *frame, int x, int y, uint8_t gray)
{
    uint8_t *byte = &frame[(y / ST75256_PAGE_HEIGHT) * ST75256_WIDTH + x];
    int      shift = (ST75256_PAGE_HEIGHT - 1 - y % ST75256_PAGE_HEIGHT) * 2;
    *byte = (*byte & ~(0x3 << shift)) | (gray << shift);
}

// Test pattern, in display coordinates (x 0..255, y 0..127):
//   x   0..127, y  0..63: four 32-column bands, white, light, dark, black.
//   x 128..255, y  0..63: a 32x32 black square on white. Crosstalk shows up
//                         as shadows in the same rows or columns.
//   x   0..127, y 64..127: 1-pixel horizontal stripes, white, light, dark,
//                         black from the top.
//   x 128..255, y 64..127: a 1-pixel black diagonal on white. Straight if the
//                         pixel order within a byte is right, a zigzag if not.
static esp_err_t draw_test_pattern(void)
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
    return st75256_write(0, 0, ST75256_WIDTH, ST75256_PAGES, frame);
}

typedef struct
{
    button_id_t     id;
    button_action_t action;
} button_event_t;

// Created by display_tune(); button events are forwarded to it.
static QueueHandle_t tune_queue;

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

static void frame_rate_str(const st75256_tuning_t *t, char *buf, size_t len, const char *dflt)
{
    if (t->frame_rate == ST75256_FRAME_RATE_DEFAULT)
    {
        snprintf(buf, len, "%s", dflt);
    }
    else
    {
        snprintf(buf, len, "%u", t->frame_rate);
    }
}

static void tune_log(const st75256_tuning_t *t, tune_param_t sel)
{
    char fr[12];
    frame_rate_str(t, fr, sizeof(fr), "default");
    ESP_LOGI(TAG, "vop=%u bias=%u booster=%u line_inv=0x%02X frame_rate=%s  [editing %s]", t->vop,
             t->bias, t->booster, t->line_inv, fr, tune_names[sel]);
}

// LEFT/RIGHT pick a setting, UP/DOWN change it (long press: by 10), PAIR
// click logs the values as C, PAIR long press restores the defaults.
static void display_tune(void)
{
    esp_err_t err = draw_test_pattern();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Test pattern failed: %s", esp_err_to_name(err));
        return;
    }

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
                    frame_rate_str(&t, fr, sizeof(fr), "ST75256_FRAME_RATE_DEFAULT");
                    ESP_LOGI(TAG, "C: {.vop = %u, .bias = %u, .booster = %u, .line_inv = 0x%02X, "
                                  ".frame_rate = %s}",
                             t.vop, t.bias, t.booster, t.line_inv, fr);
                }
                break;
        }

        if (changed)
        {
            err = st75256_set_tuning(&t);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Set tuning failed: %s", esp_err_to_name(err));
            }
        }
        tune_log(&t, sel);
    }
}
#endif

#if HW_TEST == HW_TEST_SD
// Reads a whole file into buf (truncated to len - 1, NUL-terminated).
// Returns the byte count, or -1 if it cannot be opened.
static int read_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        return -1;
    }
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

// Card detect, mount, list the root, read the old firmware's own profile,
// write/read/delete a file, then alternate display and SD access.
static void sd_test(bool display_ok)
{
    if (!sdcard_present())
    {
        ESP_LOGW(TAG, "SD: no card");
        return;
    }
    esp_err_t err = sdcard_mount();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }

    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir)
    {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
        {
            ESP_LOGI(TAG, "SD: %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        }
        closedir(dir);
    }
    else
    {
        ESP_LOGE(TAG, "SD: opendir failed");
    }

    static char buf[512];
    int         n = read_file(SDCARD_MOUNT_POINT "/self_profile/profile.json", buf, sizeof(buf));
    if (n >= 0)
    {
        ESP_LOGI(TAG, "SD: self profile.json, %d bytes shown: %s", n, buf);
    }
    else
    {
        ESP_LOGW(TAG, "SD: no /self_profile/profile.json");
    }

    // Write, read back, compare, delete.
    const char *test_path = SDCARD_MOUNT_POINT "/m6_test.txt";
    const char *text = "con-venience M6 SD test\n";
    FILE       *f = fopen(test_path, "w");
    bool        ok = f && fputs(text, f) >= 0;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    ok = ok && read_file(test_path, buf, sizeof(buf)) >= 0 && strcmp(buf, text) == 0;
    ok = (remove(test_path) == 0) && ok;
    ESP_LOGI(TAG, "SD: write/read/delete %s", ok ? "OK" : "FAILED");

    // Display and SD in turns on the shared bus. The bottom page flips
    // between black and white, so the display side is visible too.
    if (display_ok)
    {
        static uint8_t page[ST75256_WIDTH];
        int            display_errors = 0;
        int            sd_errors = 0;
        for (int i = 0; i < 50; i++)
        {
            memset(page, (i & 1) ? 0xFF : 0x00, sizeof(page));
            if (st75256_write(0, ST75256_PAGES - 1, ST75256_WIDTH, 1, page) != ESP_OK)
            {
                display_errors++;
            }
            f = fopen(test_path, "w");
            if (!f || fprintf(f, "%d\n", i) < 0 || fclose(f) != 0
                || read_file(test_path, buf, sizeof(buf)) < 0 || atoi(buf) != i)
            {
                sd_errors++;
            }
        }
        remove(test_path);
        ESP_LOGI(TAG, "SD: 50 alternating rounds, display errors %d, SD errors %d", display_errors,
                 sd_errors);
    }

    err = sdcard_unmount();
    ESP_LOGI(TAG, "SD: unmount %s", esp_err_to_name(err));
}
#endif

#if HW_TEST == HW_TEST_AUDIO
static void play(const char *what, const max98357a_note_t *notes, size_t count, uint32_t gap_ms)
{
    ESP_LOGI(TAG, "Audio: %s", what);
    esp_err_t err = max98357a_play(notes, count, gap_ms);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio: %s failed: %s", what, esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Needs +3.3V_PERIPH, which display_start() switched on.
static void audio_test(void)
{
    esp_err_t err = max98357a_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio init failed: %s", esp_err_to_name(err));
        return;
    }

    static const max98357a_note_t beep[] = {{2500, 50}};  // old startup sound
    static const max98357a_note_t melody[] = {{262, 140}, {294, 140}, {330, 400}};
    const size_t              melody_len = sizeof(melody) / sizeof(melody[0]);

    max98357a_set_waveform(MAX98357A_WAVE_SQUARE);
    play("startup beep", beep, 1, 15);
    play("melody, square", melody, melody_len, 15);
    // Small speakers barely reproduce a few hundred Hz; a square wave is
    // heard mostly through its harmonics, a sine has none. Same melody as a
    // sine at x1, x4 and x8 the frequency.
    static const max98357a_note_t melody_x4[] = {{1047, 140}, {1175, 140}, {1319, 400}};
    static const max98357a_note_t melody_x8[] = {{2093, 140}, {2349, 140}, {2637, 400}};
    max98357a_set_waveform(MAX98357A_WAVE_SINE);
    play("melody, sine", melody, melody_len, 15);
    play("melody, sine, 2 octaves up", melody_x4, melody_len, 15);
    play("melody, sine, 3 octaves up", melody_x8, melody_len, 15);

    err = max98357a_deinit();
    ESP_LOGI(TAG, "Audio: deinit %s", esp_err_to_name(err));
}
#endif

void hw_test_run(bool display_ok)
{
#if HW_TEST == HW_TEST_SLEEP
    (void)display_ok;
    sleep_test();
#elif HW_TEST == HW_TEST_DISPLAY_TUNE
    if (display_ok)
    {
        display_tune();
    }
#elif HW_TEST == HW_TEST_SD
    sd_test(display_ok);
#elif HW_TEST == HW_TEST_AUDIO
    (void)display_ok;
    audio_test();
#else
    (void)display_ok;
#endif
}

void hw_test_on_button(button_id_t id, button_action_t action)
{
#if HW_TEST == HW_TEST_DISPLAY_TUNE
    if (tune_queue)
    {
        const button_event_t ev = {.id = id, .action = action};
        xQueueSend(tune_queue, &ev, 0);
    }
#else
    (void)id;
    (void)action;
#endif
}
