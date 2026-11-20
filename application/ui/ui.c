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

// Partial rendering into a band of this many rows: 256 x 32 L8 = 8 KB.
#define UI_BUF_ROWS 32
// lv_timer_handler() is called at least this often.
#define UI_MAX_SLEEP_MS 50
#define UI_BUTTON_QUEUE_LEN 8
#define UI_LV_KEYS_LEN 8

lv_style_t ui_style_focus;

static lv_display_t *display;
static lv_indev_t   *keypad;

typedef struct
{
    button_id_t     id;
    button_action_t action;
} button_event_t;

// Button events from the button task, handled in ui_run().
static QueueHandle_t button_queue;
// The current page's key handler, or NULL for LVGL focus. LVGL task only.
static ui_key_handler_t key_handler;
// Keys for the LVGL keypad, LVGL task only.
static uint32_t lv_keys[UI_LV_KEYS_LEN];
static size_t   lv_keys_head, lv_keys_count;

static uint8_t render_buf[ST75256_WIDTH * UI_BUF_ROWS] __attribute__((aligned(4)));
// The whole screen in the panel's own 2bpp layout (8 KB). LVGL splits a
// large dirty area into chunks of buffer size / width rows, which need not
// be a multiple of the 4-row page: a chunk may start or end inside a page.
// Pixels go here row-exact, and whole pages go to the panel from here.
static uint8_t frame[ST75256_PAGES * ST75256_WIDTH];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// L8 (0 black .. 255 white) to the panel's 2bpp (0 white .. 3 black), four
// rows per byte, top row in bits 7..6.
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_L8);

    for (int32_t y = area->y1; y <= area->y2; y++)
    {
        const uint8_t *row = px + (y - area->y1) * stride;
        uint8_t       *dst = &frame[(y / ST75256_PAGE_HEIGHT) * ST75256_WIDTH + area->x1];
        const int      shift = (ST75256_PAGE_HEIGHT - 1 - y % ST75256_PAGE_HEIGHT) * 2;
        for (int32_t x = 0; x < w; x++)
        {
            uint8_t gray = 3 - (row[x] >> 6);
            dst[x] = (dst[x] & ~(0x3 << shift)) | (gray << shift);
        }
    }

    // Each page's window is contiguous in frame, so one write per page.
    for (int32_t p = area->y1 / ST75256_PAGE_HEIGHT; p <= area->y2 / ST75256_PAGE_HEIGHT; p++)
    {
        esp_err_t err =
            st75256_write(area->x1, p, w, 1, &frame[p * ST75256_WIDTH + area->x1]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Flush failed: %s", esp_err_to_name(err));
            break;
        }
    }
    lv_display_flush_ready(disp);
}

static bool lv_key_pop(uint32_t *key)
{
    if (lv_keys_count == 0)
    {
        return false;
    }
    *key = lv_keys[lv_keys_head];
    lv_keys_head = (lv_keys_head + 1) % UI_LV_KEYS_LEN;
    lv_keys_count--;
    return true;
}

static void lv_key_push(uint32_t key)
{
    if (lv_keys_count < UI_LV_KEYS_LEN)
    {
        lv_keys[(lv_keys_head + lv_keys_count) % UI_LV_KEYS_LEN] = key;
        lv_keys_count++;
    }
}

// Keypad: one key is reported as pressed, then released on the next read.
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
    if (lv_key_pop(&key))
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

// As the old firmware: UP/DOWN move, RIGHT enters (long press: delete),
// LEFT goes back. PAIR
// acts as enter for now, its long press as back; the page flow (fsm) will
// give PAIR its own meaning (pairing).
static uint32_t to_lv_key(button_id_t id, button_action_t action)
{
    switch (id)
    {
        case BUTTON_UP:
            return LV_KEY_PREV;
        case BUTTON_DOWN:
            return LV_KEY_NEXT;
        case BUTTON_LEFT:
            return LV_KEY_ESC;
        case BUTTON_RIGHT:
            // Long press: delete a friend, as the old firmware.
            return action == BUTTON_LONG_PRESS ? LV_KEY_DEL : LV_KEY_ENTER;
        case BUTTON_PAIR:
        default:
            return action == BUTTON_LONG_PRESS ? LV_KEY_ESC : LV_KEY_ENTER;
    }
}

void ui_on_button(button_id_t id, button_action_t action)
{
    if (!button_queue)
    {
        return;
    }
    const button_event_t ev = {.id = id, .action = action};
    xQueueSend(button_queue, &ev, 0);
}

void ui_set_key_handler(ui_key_handler_t handler)
{
    key_handler = handler;
}

// In the LVGL task: to the page's handler, or to LVGL's focus.
static void dispatch_buttons(void)
{
    button_event_t ev;
    while (xQueueReceive(button_queue, &ev, 0) == pdTRUE)
    {
        if (key_handler)
        {
            key_handler(ev.id, ev.action);
        }
        else
        {
            lv_key_push(to_lv_key(ev.id, ev.action));
        }
    }
}

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, UI_GRAY(UI_L8_WHITE), 0);
    return scr;
}

void ui_screen_show(lv_obj_t *scr)
{
    key_handler = NULL;
    lv_keys_count = 0;  // keys meant for the old page
    // No transition: STN smears anything that moves.
    lv_obj_t *old = lv_screen_active();
    if (old && old != scr)
    {
        // Out of the keypad group first. Otherwise deleting the focused
        // widget moves the focus to its neighbour on the dying screen, whose
        // focus callback then touches already deleted widgets (crashed when
        // opening a friend from the list).
        lv_group_t *group = lv_group_get_default();
        for (int32_t i = (int32_t)lv_group_get_obj_count(group) - 1; i >= 0; i--)
        {
            lv_obj_t *obj = lv_group_get_obj_by_index(group, i);
            if (lv_obj_get_screen(obj) == old)
            {
                lv_group_remove_obj(obj);
            }
        }
    }
    lv_screen_load(scr);
    if (old && old != scr)
    {
        // Only after the current event: pages are switched from inside
        // event callbacks of the old screen's widgets, which LVGL still
        // uses when the callback returns.
        lv_obj_delete_async(old);
    }
}

esp_err_t ui_init(void)
{
    if (display)
    {
        return ESP_ERR_INVALID_STATE;
    }
    button_queue = xQueueCreate(UI_BUTTON_QUEUE_LEN, sizeof(button_event_t));
    if (!button_queue)
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
    // Edges of circles and rounded corners would come out gray otherwise;
    // the UI is black and white (fonts are bitmaps anyway).
    lv_display_set_antialiasing(display, false);

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
        dispatch_buttons();
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
