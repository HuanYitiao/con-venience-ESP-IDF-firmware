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
    ui_title_bar(scr, 0, UI_SCREEN_W, "Clock");
    clock_time = NULL;
    clock_date = NULL;

    switch (c->status)
    {
        case UI_CLOCK_OK:
            clock_time = ui_centered_label(scr, UI_FONT_CLOCK, 40, "");
            clock_date = ui_centered_label(scr, UI_FONT_LARGE, 80, "");
            ui_clock_update(c);
            break;
        case UI_CLOCK_NOT_SET:
            // Fail-closed: never show a time the RTC cannot vouch for.
            ui_centered_label(scr, UI_FONT_LARGE, 40, "Time not set");
            ui_centered_label(scr, UI_FONT_SMALL, 64, "Settings > Wi-Fi setup");
            ui_centered_label(scr, UI_FONT_SMALL, 76, "to sync time");
            break;
        case UI_CLOCK_RTC_ERROR:
        default:
            ui_centered_label(scr, UI_FONT_LARGE, 40, "RTC error");
            ui_centered_label(scr, UI_FONT_SMALL, 64, "I2C read failed");
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
        // The password is new on every start: a phone that saved this
        // network tries the old one and never joins.
        static const char *const lines[] = {"Scan to join Wi-Fi", "", "Can't join? Remove",
                                            "the saved network", "on your phone first."};
        setup_step(scr, s->wifi_qr, "Step 1/2", lines, 5);
    }
    else if (s->step == 2)
    {
        const char *const lines[] = {"Scan to open the", "setup page", "", "Or visit:", s->url};
        setup_step(scr, s->url, "Step 2/2", lines, 5);
    }
    else
    {
        ui_centered_label(scr, UI_FONT_LARGE, 8, "Saved");
        ui_centered_label(scr, UI_FONT_SMALL, 34, s->self_id);
        ui_centered_label(scr, UI_FONT_SMALL, 50, "ID (recovery coming soon)");
        ui_centered_label(scr, UI_FONT_SMALL, 72, "LEFT to finish");
    }
    ui_screen_show(scr);
}

// ── Messages ─────────────────────────────────────────────

void ui_show_message(const char *title, const char *line)
{
    lv_obj_t *scr = ui_screen_create();
    ui_centered_label(scr, UI_FONT_LARGE, 48, title);
    if (line)
    {
        ui_centered_label(scr, UI_FONT_SMALL, 68, line);
    }
    ui_screen_show(scr);
}
