// M5: test pattern, then the buttons tune the display drive settings.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_DISPLAY_TUNE

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "st75256.h"

static const char *TAG = "hw_test";

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
void test_display_run(void)
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


void test_display_on_button(button_id_t id, button_action_t action)
{
    if (tune_queue)
    {
        const button_event_t ev = {.id = id, .action = action};
        xQueueSend(tune_queue, &ev, 0);
    }
}

#endif
