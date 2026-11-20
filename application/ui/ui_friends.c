// Friend list, as the old firmware: names on the right, the focused one
// marked, and that friend's avatar on the left. UP/DOWN move, RIGHT opens
// the friend, RIGHT long press deletes, LEFT goes back.
//
// The focused row is marked with "»" in front of the name. Inverting it (as
// menus do) puts a solid black bar in the same rows as the avatar's grays,
// which then take heavy crosstalk; of the styles tried on the panel
// (invert, frame, left bar, underline, arrow) the arrow looked best.

#include "esp_log.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui";

static ui_friend_loader_t         loader;
static const ui_friend_actions_t *actions;
static lv_obj_t                  *avatar_area;

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
    show_avatar((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_key(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    switch (lv_event_get_key(e))
    {
        case LV_KEY_ENTER:
            if (actions->open)
            {
                actions->open(index);
            }
            break;
        case LV_KEY_DEL:
            if (actions->remove)
            {
                actions->remove(index);
            }
            break;
        case LV_KEY_ESC:
            if (actions->back)
            {
                actions->back();
            }
            break;
        default:
            break;
    }
}

// An empty list has nothing to focus: LEFT comes through the key handler.
static void empty_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT && actions->back)
    {
        actions->back();
    }
}

void ui_show_friend_list(const char *const *names, int count, int focus, ui_friend_loader_t load,
                         const ui_friend_actions_t *a)
{
    lv_obj_t *scr = ui_screen_create();
    loader = load;
    actions = a;

    if (count <= 0)
    {
        lv_obj_t *label = lv_label_create(scr);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        lv_label_set_text(label, "No friends yet");
        lv_obj_align(label, LV_ALIGN_CENTER, UI_HALF / 2, 0);
        ui_screen_show(scr);
        ui_set_key_handler(empty_keys);
        return;
    }

    avatar_area = lv_obj_create(scr);
    lv_obj_remove_style_all(avatar_area);
    lv_obj_set_size(avatar_area, UI_HALF, UI_SCREEN_H);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_HALF, UI_SCREEN_H);
    lv_obj_set_pos(list, UI_HALF, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    if (focus < 0 || focus >= count)
    {
        focus = 0;
    }
    lv_obj_t *start = NULL;
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *row = ui_marker_row(list, UI_HALF, names[i]);
        lv_obj_add_event_cb(row, on_focus, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_key, LV_EVENT_KEY, (void *)(intptr_t)i);
        if (i == focus)
        {
            start = row;
        }
    }

    ui_screen_show(scr);
    lv_group_focus_obj(start);  // fires on_focus: that avatar shows
}
