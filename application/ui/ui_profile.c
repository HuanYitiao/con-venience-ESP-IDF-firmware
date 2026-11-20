// Profile pages, as the old firmware: the link list, a link's QR code, the
// friend delete confirmation, and the blank standby screen.

#include <string.h>

#include "battery.h"
#include "ui.h"
#include "ui_priv.h"

#define QR_SIZE 116  // up to version 3 at 4 px per module, between the bands

// ── Links ────────────────────────────────────────────────

static void (*links_open)(int index);
static void (*links_back)(void);

static void on_link_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER && links_open)
    {
        links_open((int)(intptr_t)lv_event_get_user_data(e));
    }
    else if (key == LV_KEY_ESC && links_back)
    {
        links_back();
    }
}

static void no_links_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT && links_back)
    {
        links_back();
    }
}

// Full width and no avatar beside it, so rows invert, as in the menu.
void ui_show_links(const contact_t *c, int focus, void (*open)(int index), void (*back)(void))
{
    links_open = open;
    links_back = back;
    lv_obj_t *scr = ui_screen_create();

    if (c->link_count == 0)
    {
        ui_centered_label(scr, UI_FONT_LARGE, 56, "No links");
        ui_screen_show(scr);
        ui_set_key_handler(no_links_keys);
        return;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (focus < 0 || focus >= c->link_count)
    {
        focus = 0;
    }
    lv_obj_t *start = NULL;
    for (int i = 0; i < c->link_count; i++)
    {
        lv_obj_t *row = ui_inverted_row(list, UI_SCREEN_W, c->links[i].platform);
        lv_obj_add_event_cb(row, on_link_key, LV_EVENT_KEY, (void *)(intptr_t)i);
        if (i == focus)
        {
            start = row;
        }
    }
    ui_screen_show(scr);
    lv_group_focus_obj(start);
}

// ── QR code ──────────────────────────────────────────────

void ui_show_qr(const char *text)
{
    lv_obj_t *scr = ui_screen_create();
    ui_side_bands(scr, BATTERY_LEVELS);  // full, as the old firmware
    lv_obj_t *code = lv_qrcode_create(scr);
    lv_qrcode_set_size(code, QR_SIZE);
    lv_qrcode_set_dark_color(code, lv_color_black());
    lv_qrcode_set_light_color(code, lv_color_white());
    lv_qrcode_update(code, text, strlen(text));
    lv_obj_center(code);
    ui_screen_show(scr);
}

// ── Delete confirmation ──────────────────────────────────

static void (*delete_cancel)(void);
static void (*delete_confirm)(void);

static void on_delete_key(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    bool     is_delete = (bool)(intptr_t)lv_event_get_user_data(e);
    if (key == LV_KEY_ESC || (key == LV_KEY_ENTER && !is_delete))
    {
        if (delete_cancel)
        {
            delete_cancel();
        }
    }
    else if (key == LV_KEY_ENTER && is_delete && delete_confirm)
    {
        delete_confirm();
    }
}

// Avatar left; right: title, name, and the two choices, marked with "»"
// as they sit beside the avatar.
void ui_show_delete_confirm(const contact_t *c, void (*cancel)(void), void (*confirm)(void))
{
    delete_cancel = cancel;
    delete_confirm = confirm;
    lv_obj_t *scr = ui_screen_create();

    const lv_image_dsc_t *avatar = ui_avatar_image(c, false);
    if (avatar)
    {
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, avatar);
        lv_obj_set_pos(img, (UI_HALF - c->avatar_res) / 2, (UI_HALF - c->avatar_res) / 2);
    }

    ui_title_bar(scr, UI_HALF, UI_HALF, "Delete friend?");
    lv_obj_t *name = lv_label_create(scr);
    lv_obj_set_style_text_font(name, UI_FONT_LARGE, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, UI_HALF - 8);
    lv_label_set_text(name, c->name);
    lv_obj_set_pos(name, UI_HALF + 4, 26);

    static const char *const items[2] = {"Cancel", "Delete"};
    lv_obj_t                *first = NULL;
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *row = ui_marker_row(scr, UI_HALF, items[i]);
        lv_obj_set_pos(row, UI_HALF, 72 + i * UI_ROW_H);
        lv_obj_add_event_cb(row, on_delete_key, LV_EVENT_KEY, (void *)(intptr_t)(i == 1));
        if (!first)
        {
            first = row;
        }
    }
    ui_screen_show(scr);
    lv_group_focus_obj(first);  // starts on Cancel, as the old firmware
}

// ── Standby ──────────────────────────────────────────────

void ui_show_standby(void)
{
    ui_screen_show(ui_screen_create());
}
