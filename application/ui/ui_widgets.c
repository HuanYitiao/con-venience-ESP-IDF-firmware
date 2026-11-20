// Small building blocks shared by the pages.

#include "ui_priv.h"

lv_obj_t *ui_centered_label(lv_obj_t *parent, const lv_font_t *font, int32_t y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

lv_obj_t *ui_title_bar(lv_obj_t *parent, int32_t x, int32_t w, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_pos(bar, x, 0);
    lv_obj_set_size(bar, w, UI_TITLE_H);
    lv_obj_t *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, w - 12);
    lv_label_set_text(label, title);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
    return bar;
}

// ── Rows marked with "»" ─────────────────────────────────

static void marker_show(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_set_hidden(lv_obj_get_child(row, 0), false);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);  // no glide on the STN
}

static void marker_hide(lv_event_t *e)
{
    lv_obj_set_hidden(lv_obj_get_child(lv_event_get_target(e), 0), true);
}

lv_obj_t *ui_marker_row(lv_obj_t *parent, int32_t w, const char *text)
{
    lv_obj_t *row = lv_button_create(parent);  // joins the default group
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, UI_ROW_H);
    lv_obj_add_event_cb(row, marker_show, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(row, marker_hide, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *marker = lv_label_create(row);  // child 0
    lv_obj_set_style_text_font(marker, UI_FONT_LARGE, 0);
    lv_label_set_text(marker, "»");
    lv_obj_align(marker, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_hidden(marker, true);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, w - 14);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(label, text);
    return row;
}

// ── Inverted rows ────────────────────────────────────────

static void scroll_into_view(lv_event_t *e)
{
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_OFF);
}

lv_obj_t *ui_inverted_row(lv_obj_t *parent, int32_t w, const char *text)
{
    lv_obj_t *row = lv_button_create(parent);  // joins the default group
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &ui_style_focus, LV_STATE_FOCUSED);
    lv_obj_set_size(row, w, UI_ROW_H);
    lv_obj_add_event_cb(row, scroll_into_view, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, UI_FONT_LARGE, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, w - 12);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
    lv_label_set_text(label, text);
    return row;
}
