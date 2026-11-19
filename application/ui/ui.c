#include "ui.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "st75256.h"
#include "ui_priv.h"

static const char *TAG = "ui";

// Partial rendering into a band of this many rows (a multiple of the
// panel's 4-row page): 256 x 32 L8 = 8 KB, plus 2 KB for the 2bpp copy.
#define UI_BUF_ROWS 32
// lv_timer_handler() is called at least this often.
#define UI_MAX_SLEEP_MS 50
#define UI_KEY_QUEUE_LEN 8

lv_style_t ui_style_focus;

static lv_display_t *display;
static lv_indev_t   *keypad;
static QueueHandle_t key_queue;

static uint8_t render_buf[ST75256_WIDTH * UI_BUF_ROWS] __attribute__((aligned(4)));
static uint8_t panel_buf[ST75256_WIDTH * UI_BUF_ROWS / ST75256_PAGE_HEIGHT];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// The panel is written in pages of 4 rows: widen every dirty area to whole pages.
static void round_to_pages(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->y1 &= ~(ST75256_PAGE_HEIGHT - 1);
    area->y2 |= ST75256_PAGE_HEIGHT - 1;
}

// L8 (0 black .. 255 white) to the panel's 2bpp (0 white .. 3 black), four
// rows per byte, top row in bits 7..6.
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t pages = lv_area_get_height(area) / ST75256_PAGE_HEIGHT;
    const int32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_L8);

    for (int32_t p = 0; p < pages; p++)
    {
        const uint8_t *rows = px + p * ST75256_PAGE_HEIGHT * stride;
        for (int32_t x = 0; x < w; x++)
        {
            uint8_t byte = 0;
            for (int r = 0; r < ST75256_PAGE_HEIGHT; r++)
            {
                uint8_t gray = 3 - (rows[r * stride + x] >> 6);
                byte |= gray << ((ST75256_PAGE_HEIGHT - 1 - r) * 2);
            }
            panel_buf[p * w + x] = byte;
        }
    }

    esp_err_t err = st75256_write(area->x1, area->y1 / ST75256_PAGE_HEIGHT, w, pages, panel_buf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Flush failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(disp);
}

// Keypad: one queued key is reported as pressed, then released on the next read.
static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last_key;
    static bool     pressed;

    data->key = last_key;
    if (pressed)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        pressed = false;
        return;
    }
    uint32_t key;
    if (xQueueReceive(key_queue, &key, 0) == pdTRUE)
    {
        last_key = key;
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
        pressed = true;
        data->continue_reading = true;  // deliver the release in the same pass
        return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

// UP/DOWN move the focus, LEFT/RIGHT go to the focused widget, PAIR is
// enter, PAIR long press is escape. Other long presses act as clicks for
// now; the page flow (fsm) will give them their own meaning.
void ui_on_button(button_id_t id, button_action_t action)
{
    if (!key_queue)
    {
        return;
    }
    uint32_t key;
    switch (id)
    {
        case BUTTON_UP:
            key = LV_KEY_PREV;
            break;
        case BUTTON_DOWN:
            key = LV_KEY_NEXT;
            break;
        case BUTTON_LEFT:
            key = LV_KEY_LEFT;
            break;
        case BUTTON_RIGHT:
            key = LV_KEY_RIGHT;
            break;
        case BUTTON_PAIR:
        default:
            key = action == BUTTON_LONG_PRESS ? LV_KEY_ESC : LV_KEY_ENTER;
            break;
    }
    xQueueSend(key_queue, &key, 0);
}

esp_err_t ui_init(void)
{
    if (display)
    {
        return ESP_ERR_INVALID_STATE;
    }
    key_queue = xQueueCreate(UI_KEY_QUEUE_LEN, sizeof(uint32_t));
    if (!key_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_tick_set_cb(tick_ms);

    display = lv_display_create(ST75256_WIDTH, ST75256_HEIGHT);
    if (!display)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_L8);
    lv_display_set_buffers(display, render_buf, NULL, sizeof(render_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, round_to_pages, LV_EVENT_INVALIDATE_AREA, NULL);

    // Black and white widgets; focus is shown by inverting.
    // Black and white widgets with the 6x12 bitmap font.
    lv_display_set_theme(display, lv_theme_mono_init(display, false, UI_FONT_SMALL));

    lv_style_init(&ui_style_focus);
    lv_style_set_bg_opa(&ui_style_focus, LV_OPA_COVER);
    lv_style_set_bg_color(&ui_style_focus, lv_color_black());
    lv_style_set_text_color(&ui_style_focus, lv_color_white());

    keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read);
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_set_group(keypad, group);
    return ESP_OK;
}

void ui_run(void)
{
    ESP_LOGI(TAG, "Running");
    while (true)
    {
        uint32_t ms = lv_timer_handler();
        if (ms > UI_MAX_SLEEP_MS)
        {
            ms = UI_MAX_SLEEP_MS;
        }
        // At least one tick, so the idle task (and its watchdog) gets to run.
        TickType_t ticks = pdMS_TO_TICKS(ms);
        vTaskDelay(ticks > 0 ? ticks : 1);
    }
}
