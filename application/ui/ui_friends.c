// Friend list, as the old firmware: names on the right, the focused one
// marked, and that friend's avatar on the left. UP/DOWN move the focus.
// The list scrolls without animation: on the STN any glide smears.

#include <string.h>

#include "esp_log.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui";

#define HALF 128
#define ROW_H 16  // 7x14 bold plus a pixel above and below

static ui_friend_loader_t loader;
static lv_obj_t          *avatar_area;

// The focused row is marked with "»" in front of the name. Inverting the
// row (as menus do) puts a solid black bar in the same rows as the avatar's
// grays, which then take heavy crosstalk; of the styles tried on the panel
// (invert, frame, left bar, underline, arrow) the arrow looked best.
static void set_marker(lv_obj_t *row, bool on)
{
    lv_obj_set_hidden(lv_obj_get_child(row, 0), !on);  // child 0: the "»" label
}

static void on_defocus(lv_event_t *e)
{
    set_marker(lv_event_get_target(e), false);
}

// The focused friend's avatar, or "No avatar".
static void show_avatar(int index)
{
    static contact_t c;  // 4.8 KB: not on the stack
    lv_obj_clean(avatar_area);
    const lv_image_dsc_t *img = NULL;
    if (loader(index, &c) == ESP_OK)
    {
        img = ui_avatar_image(&c, false);
    }
    else
    {
        ESP_LOGW(TAG, "Friend %d does not load", index);
    }
    if (img)
    {
        lv_obj_t *obj = lv_image_create(avatar_area);
        lv_image_set_src(obj, img);
        lv_obj_center(obj);
    }
    else
    {
        lv_obj_t *label = lv_label_create(avatar_area);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        lv_label_set_text(label, "No avatar");
        lv_obj_center(label);
    }
}

static void on_focus(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    set_marker(row, true);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    show_avatar((int)(intptr_t)lv_event_get_user_data(e));
}

void ui_show_friend_list(const char *const *names, int count, ui_friend_loader_t load)
{
    lv_obj_t *scr = ui_screen_create();
    loader = load;

    if (count <= 0)
    {
        lv_obj_t *label = lv_label_create(scr);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        lv_label_set_text(label, "No friends yet");
        lv_obj_align(label, LV_ALIGN_CENTER, HALF / 2, 0);
        ui_screen_show(scr);
        return;
    }

    avatar_area = lv_obj_create(scr);
    lv_obj_remove_style_all(avatar_area);
    lv_obj_set_size(avatar_area, HALF, HALF);
    lv_obj_set_pos(avatar_area, 0, 0);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, HALF, HALF);
    lv_obj_set_pos(list, HALF, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *first = NULL;
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, HALF, ROW_H);
        lv_obj_add_event_cb(row, on_focus, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_defocus, LV_EVENT_DEFOCUSED, NULL);

        lv_obj_t *marker = lv_label_create(row);  // child 0
        lv_obj_set_style_text_font(marker, UI_FONT_LARGE, 0);
        lv_label_set_text(marker, "\u00bb");
        lv_obj_align(marker, LV_ALIGN_LEFT_MID, 1, 0);
        lv_obj_set_hidden(marker, true);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(label, HALF - 14);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_text(label, names[i]);
        if (!first)
        {
            first = row;
        }
    }

    ui_screen_show(scr);
    lv_group_focus_obj(first);  // fires on_focus: the first avatar shows
}
