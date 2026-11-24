// The menu: main (My Profile, Friends, Settings), Settings (Wi-Fi setup,
// Clock, Auto sleep, Reset Profile) and the reset confirmation, as the old
// firmware. Entries that leave the menu call back into the caller.
//
// Built the way LVGL 9.6 recommends now that lv_menu is deprecated (see its
// lv_example_menu_navigation): one container per page, only one of them
// visible, and a title bar where a page has a title. The keypad group skips hidden pages, so UP/DOWN
// stay on the visible one. A new page starts on its first entry; back
// returns to the entry that opened it.

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui";

#define ROW_H 16
#define TITLE_H 16
#define SCREEN_H 128
#define DEPTH_MAX 3

typedef enum
{
    PAGE_MAIN,
    PAGE_SETTINGS,
    PAGE_RESET,
    PAGE_COUNT,
} page_id_t;

// Only the reset confirmation has a title bar, as the old firmware; the
// main and Settings pages are plain lists.
static const char *const page_titles[PAGE_COUNT] = {NULL, NULL, "Reset Profile"};

static const ui_menu_actions_t *actions;
static lv_obj_t                *title_bar;
static lv_obj_t                *title;
static lv_obj_t                *pages[PAGE_COUNT];
static bool                     auto_sleep_on;

// Where the menu was left: the page path and the focused entry on each
// level, so it reopens there after a visit to the card, clock, ...
static page_id_t path[DEPTH_MAX];
static int       focus_index[DEPTH_MAX];
static int       depth;  // 0: main page

void ui_menu_rewind(void)
{
    depth = 0;
    focus_index[0] = 0;
}

static lv_obj_t *entry_at(page_id_t page, int index)
{
    // Entries are the focusable children; labels and text are not.
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(pages[page]); i++)
    {
        lv_obj_t *child = lv_obj_get_child(pages[page], i);
        if (lv_obj_get_group(child))
        {
            if (n++ == index)
            {
                return child;
            }
        }
    }
    return NULL;
}

static int index_of(lv_obj_t *entry)
{
    lv_obj_t *page = lv_obj_get_parent(entry);
    int       n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++)
    {
        lv_obj_t *child = lv_obj_get_child(page, i);
        if (child == entry)
        {
            return n;
        }
        if (lv_obj_get_group(child))
        {
            n++;
        }
    }
    return 0;
}

// Shows the page at the current depth and focuses its remembered entry.
static void show_depth(void)
{
    for (int i = 0; i < PAGE_COUNT; i++)
    {
        lv_obj_set_hidden(pages[i], i != (int)path[depth]);
    }
    const char *t = page_titles[path[depth]];
    lv_obj_set_hidden(title_bar, t == NULL);
    lv_label_set_text(title, t ? t : "");
    lv_obj_t *entry = entry_at(path[depth], focus_index[depth]);
    if (!entry)
    {
        entry = entry_at(path[depth], 0);
    }
    if (entry)
    {
        lv_group_focus_obj(entry);
        lv_obj_scroll_to_view(entry, LV_ANIM_OFF);
    }
}

static void open_page(lv_obj_t *from, page_id_t page)
{
    if (depth + 1 >= DEPTH_MAX)
    {
        return;
    }
    focus_index[depth] = index_of(from);
    depth++;
    path[depth] = page;
    focus_index[depth] = 0;
    show_depth();
}

static void go_back(void)
{
    if (depth == 0)
    {
        if (actions->exit)
        {
            actions->exit();
        }
        return;
    }
    depth--;
    show_depth();
}

// Remembers the entry before leaving the menu, so it reopens there.
static void leave(lv_obj_t *from, void (*action)(void))
{
    focus_index[depth] = index_of(from);
    if (action)
    {
        action();
    }
}

// ── Entries ──────────────────────────────────────────────

typedef enum
{
    ENTRY_PROFILE,
    ENTRY_FRIENDS,
    ENTRY_SETTINGS,
    ENTRY_WIFI,
    ENTRY_CLOCK,
    ENTRY_AUTO_SLEEP,
    ENTRY_RESET,
    ENTRY_RESET_CANCEL,
    ENTRY_RESET_CONFIRM,
} entry_id_t;

static void set_auto_sleep_text(lv_obj_t *entry)
{
    lv_label_set_text(lv_obj_get_child(entry, 0),
                      auto_sleep_on ? "Auto sleep: On" : "Auto sleep: Off");
}

static void on_click(lv_event_t *e)
{
    lv_obj_t  *entry = lv_event_get_target(e);
    entry_id_t id = (entry_id_t)(intptr_t)lv_event_get_user_data(e);
    switch (id)
    {
        case ENTRY_PROFILE:
            leave(entry, actions->profile);
            break;
        case ENTRY_FRIENDS:
            leave(entry, actions->friends);
            break;
        case ENTRY_SETTINGS:
            open_page(entry, PAGE_SETTINGS);
            break;
        case ENTRY_WIFI:
            leave(entry, actions->wifi_setup);
            break;
        case ENTRY_CLOCK:
            leave(entry, actions->clock);
            break;
        case ENTRY_AUTO_SLEEP:
            // Toggles in place, as the old firmware.
            auto_sleep_on = !auto_sleep_on;
            set_auto_sleep_text(entry);
            if (actions->auto_sleep)
            {
                actions->auto_sleep(auto_sleep_on);
            }
            break;
        case ENTRY_RESET:
            open_page(entry, PAGE_RESET);  // opens on Cancel
            break;
        case ENTRY_RESET_CANCEL:
            go_back();
            break;
        case ENTRY_RESET_CONFIRM:
            leave(entry, actions->reset_profile);
            break;
    }
}

static void on_key(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_ESC)
    {
        go_back();
    }
}

static lv_obj_t *add_entry(page_id_t page, const char *text, entry_id_t id)
{
    lv_obj_t *entry = lv_obj_create(pages[page]);
    lv_obj_remove_style_all(entry);
    lv_obj_add_style(entry, &ui_style_focus, LV_STATE_FOCUSED);
    lv_obj_set_size(entry, lv_pct(100), ROW_H);
    lv_obj_set_style_pad_left(entry, 6, 0);
    lv_obj_set_clickable(entry, true);
    lv_group_add_obj(lv_group_get_default(), entry);
    lv_obj_add_event_cb(entry, on_click, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_add_event_cb(entry, on_key, LV_EVENT_KEY, NULL);

    lv_obj_t *label = lv_label_create(entry);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_label_set_text(label, text);
    lv_obj_set_align(label, LV_ALIGN_LEFT_MID);
    return entry;
}

static lv_obj_t *add_text(page_id_t page, const char *text)
{
    lv_obj_t *label = lv_label_create(pages[page]);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_page(lv_obj_t *content)
{
    lv_obj_t *page = lv_obj_create(content);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    return page;
}

void ui_show_menu(const ui_menu_actions_t *a, uint16_t battery_mv, bool auto_sleep)
{
    actions = a;
    auto_sleep_on = auto_sleep;
    lv_obj_t *scr = ui_screen_create();

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, lv_pct(100), SCREEN_H);

    pages[PAGE_MAIN] = add_page(content);
    add_entry(PAGE_MAIN, "My Profile", ENTRY_PROFILE);
    add_entry(PAGE_MAIN, "Friends", ENTRY_FRIENDS);
    add_entry(PAGE_MAIN, "Settings", ENTRY_SETTINGS);
    char bat[16];
    if (battery_mv > 0)
    {
        snprintf(bat, sizeof(bat), "BAT %u.%02uV", battery_mv / 1000, (battery_mv % 1000) / 10);
    }
    else
    {
        snprintf(bat, sizeof(bat), "BAT --");
    }
    lv_obj_t *bat_label = add_text(PAGE_MAIN, bat);
    lv_obj_set_style_pad_top(bat_label, 8, 0);
    // The version a bug report needs: the git tag, or tag-commits-hash
    // (-dirty) for a build between releases.
    add_text(PAGE_MAIN, esp_app_get_description()->version);

    pages[PAGE_SETTINGS] = add_page(content);
    add_entry(PAGE_SETTINGS, "Wi-Fi setup", ENTRY_WIFI);
    add_entry(PAGE_SETTINGS, "Clock", ENTRY_CLOCK);
    set_auto_sleep_text(add_entry(PAGE_SETTINGS, "", ENTRY_AUTO_SLEEP));
    add_entry(PAGE_SETTINGS, "Reset Profile", ENTRY_RESET);

    pages[PAGE_RESET] = add_page(content);
    lv_obj_set_style_pad_top(pages[PAGE_RESET], TITLE_H, 0);  // room for the title bar
    lv_obj_t *note = add_text(PAGE_RESET, "Profile and friends move to backup folders on the SD card.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_ver(note, 4, 0);
    add_entry(PAGE_RESET, "Cancel", ENTRY_RESET_CANCEL);
    add_entry(PAGE_RESET, "Reset & reboot", ENTRY_RESET_CONFIRM);

    // Title bar over the top of the page, inverted as the old firmware's.
    // LEFT is back, so there is no back button.
    title_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(title_bar);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_black(), 0);
    lv_obj_set_size(title_bar, lv_pct(100), TITLE_H);
    title = lv_label_create(title_bar);
    lv_obj_set_style_text_font(title, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 6, 0);

    if (depth == 0)
    {
        path[0] = PAGE_MAIN;
    }
    ui_screen_show(scr);
    show_depth();
    ESP_LOGI(TAG, "Menu, depth %d", depth);
}
