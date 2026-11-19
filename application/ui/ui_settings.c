// Pages reached from Settings, as the old firmware: the clock, the Wi-Fi
// setup steps, and plain messages ("Profile reset" / "Rebooting...").

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "ui_priv.h"

#define SCREEN_W 256
#define HALF 128
#define TITLE_H 16
#define QR_SIZE 116  // a version 3 code at 4 px per module, inside the left half

static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *font, int32_t y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

// Inverted title bar across the top, as the old firmware's.
static void title_bar(lv_obj_t *scr, const char *title)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_size(bar, SCREEN_W, TITLE_H);
    lv_obj_t *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, title);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
}

// ── Clock ────────────────────────────────────────────────

static lv_obj_t *clock_time;
static lv_obj_t *clock_date;

void ui_clock_update(const ui_clock_t *c)
{
    if (!clock_time || c->status != UI_CLOCK_OK)
    {
        return;
    }
    lv_label_set_text_fmt(clock_time, "%02u:%02u:%02u", c->hours, c->minutes, c->seconds);
    lv_label_set_text_fmt(clock_date, "%04u-%02u-%02u", c->year, c->month, c->day);
}

void ui_show_clock(const ui_clock_t *c)
{
    lv_obj_t *scr = ui_screen_create();
    title_bar(scr, "Clock");
    clock_time = NULL;
    clock_date = NULL;

    switch (c->status)
    {
        case UI_CLOCK_OK:
            clock_time = centered(scr, UI_FONT_CLOCK, 40, "");
            clock_date = centered(scr, UI_FONT_LARGE, 80, "");
            ui_clock_update(c);
            break;
        case UI_CLOCK_NOT_SET:
            // Fail-closed: never show a time the RTC cannot vouch for.
            centered(scr, UI_FONT_LARGE, 40, "Time not set");
            centered(scr, UI_FONT_SMALL, 64, "Settings > Wi-Fi setup");
            centered(scr, UI_FONT_SMALL, 76, "to sync time");
            break;
        case UI_CLOCK_RTC_ERROR:
        default:
            centered(scr, UI_FONT_LARGE, 40, "RTC error");
            centered(scr, UI_FONT_SMALL, 64, "I2C read failed");
            break;
    }
    ui_screen_show(scr);
}

// ── Wi-Fi setup ──────────────────────────────────────────

// QR code on the left half, title and instructions on the right.
static void setup_step(lv_obj_t *scr, const char *qr, const char *title, const char *const *lines,
                       int count)
{
    lv_obj_t *code = lv_qrcode_create(scr);
    lv_qrcode_set_size(code, QR_SIZE);
    lv_qrcode_set_dark_color(code, lv_color_black());
    lv_qrcode_set_light_color(code, lv_color_white());
    lv_qrcode_update(code, qr, strlen(qr));
    lv_obj_set_pos(code, (HALF - QR_SIZE) / 2, (HALF - QR_SIZE) / 2);

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, UI_FONT_LARGE, 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, HALF + 4, 16);
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, HALF - 8);
        lv_label_set_text(l, lines[i]);
        lv_obj_set_pos(l, HALF + 4, 40 + i * 12);
    }
}

void ui_show_wifi_setup(const ui_wifi_setup_t *s)
{
    lv_obj_t *scr = ui_screen_create();
    if (s->step == 1)
    {
        static const char *const lines[] = {"Scan to join Wi-Fi"};
        setup_step(scr, s->wifi_qr, "Step 1/2", lines, 1);
    }
    else if (s->step == 2)
    {
        const char *const lines[] = {"Scan to open the", "setup page", "", "Or visit:", s->url};
        setup_step(scr, s->url, "Step 2/2", lines, 5);
    }
    else
    {
        centered(scr, UI_FONT_LARGE, 8, "Saved");
        centered(scr, UI_FONT_SMALL, 34, s->self_id);
        centered(scr, UI_FONT_SMALL, 50, "ID (recovery coming soon)");
        centered(scr, UI_FONT_SMALL, 72, "Reboot to apply");
    }
    ui_screen_show(scr);
}

// ── Messages ─────────────────────────────────────────────

void ui_show_message(const char *title, const char *line)
{
    lv_obj_t *scr = ui_screen_create();
    centered(scr, UI_FONT_LARGE, 48, title);
    if (line)
    {
        centered(scr, UI_FONT_SMALL, 68, line);
    }
    ui_screen_show(scr);
}
