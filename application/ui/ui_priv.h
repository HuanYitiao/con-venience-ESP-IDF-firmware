#pragma once

#include <stdbool.h>

#include "contacts.h"
#include "lvgl.h"

// misc-fixed bitmap fonts, black and white only: 7x14 bold for names and
// menus, 6x12 for everything else.
LV_FONT_DECLARE(ui_font_7x14b);
LV_FONT_DECLARE(ui_font_6x12);
LV_FONT_DECLARE(ui_font_12x24);

#define UI_FONT_LARGE (&ui_font_7x14b)
#define UI_FONT_SMALL (&ui_font_6x12)
#define UI_FONT_CLOCK (&ui_font_12x24)  // ASCII only

// The four levels the panel shows, as L8. LVGL colours are mapped to the
// nearest of them in the flush callback.
#define UI_L8_WHITE 255
#define UI_L8_LIGHT 170
#define UI_L8_DARK 85
#define UI_L8_BLACK 0

#define UI_GRAY(l8) lv_color_make((l8), (l8), (l8))

#define UI_SCREEN_W 256
#define UI_SCREEN_H 128
#define UI_HALF 128     // avatar on one half, text on the other
#define UI_TITLE_H 16   // inverted title bars
#define UI_ROW_H 16     // list rows: 7x14 bold plus a pixel above and below

// Focused widgets: black background, white text (the mono theme's outline
// gets clipped by containers).
extern lv_style_t ui_style_focus;

// The avatar as an image, in a buffer shared by all pages; NULL without one.
// rotate180: for the home page, which faces the person in front.
const lv_image_dsc_t *ui_avatar_image(const contact_t *c, bool rotate180);

// A blank white screen, and showing one (the old screen is deleted).
lv_obj_t *ui_screen_create(void);
void      ui_screen_show(lv_obj_t *scr);

// A label centered horizontally, top at y.
lv_obj_t *ui_centered_label(lv_obj_t *parent, const lv_font_t *font, int32_t y, const char *text);

// Black bar with white 7x14 bold text, at the top from x, w wide.
lv_obj_t *ui_title_bar(lv_obj_t *parent, int32_t x, int32_t w, const char *title);

// A focusable list row (joins the default group). Marked: "»" in front when
// focused, for rows beside an avatar (an inverted row there puts heavy
// crosstalk on the avatar). Inverted: black when focused, elsewhere.
// Focus scrolls the row into view without animation.
lv_obj_t *ui_marker_row(lv_obj_t *parent, int32_t w, const char *text);
lv_obj_t *ui_inverted_row(lv_obj_t *parent, int32_t w, const char *text);

// The dithered side bands of the home page (battery gauge), also around QR
// codes. battery_level: 0..BATTERY_LEVELS.
void ui_side_bands(lv_obj_t *scr, uint8_t battery_level);
