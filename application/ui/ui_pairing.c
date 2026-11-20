// Pairing pages, as the old firmware: waiting for contact, connecting (a
// ripple next to the own avatar), and the transfer progress. Two changes
// for the STN: the ripple steps every 400 ms instead of 200 (a jump leaves
// a ghost for almost 0.5 s), and the progress bar is a black frame with a
// black fill instead of a light gray track, which the fill's black would
// shift through crosstalk.

#include <stdio.h>

#include "ui.h"
#include "ui_priv.h"

#define SCREEN_W 256
#define SCREEN_H 128
#define HALF 128

// Ripple: half circles to the right of the screen's center.
#define RIPPLE_RINGS 4
#define RIPPLE_R0 20
#define RIPPLE_STEP 6
#define RIPPLE_FRAME_MS 400

#define BAR_X 28
#define BAR_Y 78
#define BAR_W (SCREEN_W - 2 * BAR_X)
#define BAR_H 12
// Frame, then a white gap, then the fill. Children sit inside the parent's
// border and padding, so the fill's box is the bar's content area.
#define BAR_BORDER 1
#define BAR_GAP 1
#define BAR_FILL_W (BAR_W - 2 * (BAR_BORDER + BAR_GAP))

static lv_obj_t *centered(lv_obj_t *parent, const lv_font_t *font, int32_t y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void own_avatar(lv_obj_t *scr, const contact_t *self)
{
    const lv_image_dsc_t *img = ui_avatar_image(self, false);
    if (img)
    {
        lv_obj_t *obj = lv_image_create(scr);
        lv_image_set_src(obj, img);
        lv_obj_set_pos(obj, (HALF - self->avatar_res) / 2, (HALF - self->avatar_res) / 2);
    }
}

// ── Waiting for contact ──────────────────────────────────

void ui_show_pairing_wait(const contact_t *self)
{
    lv_obj_t *scr = ui_screen_create();
    own_avatar(scr, self);
    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_remove_style_all(right);
    lv_obj_set_pos(right, HALF, 0);
    lv_obj_set_size(right, HALF, SCREEN_H);
    lv_obj_t *label = lv_label_create(right);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_label_set_text(label, "Touch devices");
    lv_obj_center(label);
    ui_screen_show(scr);
}

// ── Connecting ───────────────────────────────────────────

static lv_obj_t *rings[RIPPLE_RINGS];
static int       ripple_frame;

// Three of the four rings show; the gap moves outward, as the old firmware.
static void ripple_step(lv_timer_t *t)
{
    ripple_frame = (ripple_frame + 1) % RIPPLE_RINGS;
    for (int i = 0; i < RIPPLE_RINGS; i++)
    {
        lv_obj_set_hidden(rings[i], i == (ripple_frame + RIPPLE_RINGS - 1) % RIPPLE_RINGS);
    }
}

static void on_ripple_delete(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

void ui_show_pairing_connecting(const contact_t *self)
{
    lv_obj_t *scr = ui_screen_create();
    own_avatar(scr, self);

    // The right half clips the circles to their right halves.
    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_remove_style_all(area);
    lv_obj_set_pos(area, HALF, 0);
    lv_obj_set_size(area, HALF, SCREEN_H);
    for (int i = 0; i < RIPPLE_RINGS; i++)
    {
        int32_t   r = RIPPLE_R0 + i * RIPPLE_STEP;
        lv_obj_t *ring = lv_obj_create(area);
        lv_obj_remove_style_all(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_black(), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_size(ring, 2 * r + 1, 2 * r + 1);
        lv_obj_set_pos(ring, -r, SCREEN_H / 2 - r);
        rings[i] = ring;
    }
    ripple_frame = 0;
    lv_timer_t *timer = lv_timer_create(ripple_step, RIPPLE_FRAME_MS, NULL);
    ripple_step(timer);
    // The timer goes with the page.
    lv_obj_add_event_cb(area, on_ripple_delete, LV_EVENT_DELETE, timer);
    ui_screen_show(scr);
}

// ── Transfer ─────────────────────────────────────────────

static lv_obj_t *transfer_pct;
static lv_obj_t *transfer_fill;
static lv_obj_t *transfer_bar;

void ui_pairing_transfer_update(int percent)
{
    if (!transfer_pct)
    {
        return;
    }
    bool known = percent >= 0;
    lv_obj_set_hidden(transfer_bar, !known);
    if (!known)
    {
        // No honest total: no made-up bar, as the old firmware.
        lv_label_set_text(transfer_pct, "...");
        return;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    lv_label_set_text_fmt(transfer_pct, "%d%%", percent);
    lv_obj_set_width(transfer_fill, BAR_FILL_W * percent / 100);
}

static void on_transfer_delete(lv_event_t *e)
{
    transfer_pct = NULL;
}

void ui_show_pairing_transfer(int percent)
{
    lv_obj_t *scr = ui_screen_create();
    centered(scr, UI_FONT_LARGE, 26, "Receiving");
    transfer_pct = centered(scr, UI_FONT_LARGE, 48, "");
    lv_obj_add_event_cb(transfer_pct, on_transfer_delete, LV_EVENT_DELETE, NULL);

    transfer_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(transfer_bar);
    lv_obj_set_style_border_width(transfer_bar, BAR_BORDER, 0);
    lv_obj_set_style_border_color(transfer_bar, lv_color_black(), 0);
    lv_obj_set_style_border_opa(transfer_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(transfer_bar, BAR_GAP, 0);
    lv_obj_set_pos(transfer_bar, BAR_X, BAR_Y);
    lv_obj_set_size(transfer_bar, BAR_W, BAR_H);

    transfer_fill = lv_obj_create(transfer_bar);
    lv_obj_remove_style_all(transfer_fill);
    lv_obj_set_style_bg_opa(transfer_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(transfer_fill, lv_color_black(), 0);
    lv_obj_set_pos(transfer_fill, 0, 0);
    lv_obj_set_size(transfer_fill, 0, lv_pct(100));

    ui_pairing_transfer_update(percent);
    ui_screen_show(scr);
}
