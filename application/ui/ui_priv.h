#pragma once

#include <stdbool.h>

#include "contacts.h"
#include "lvgl.h"

// misc-fixed bitmap fonts, black and white only: 7x14 bold for names and
// menus, 6x12 for everything else.
LV_FONT_DECLARE(ui_font_7x14b);
LV_FONT_DECLARE(ui_font_6x12);

#define UI_FONT_LARGE (&ui_font_7x14b)
#define UI_FONT_SMALL (&ui_font_6x12)

// The four levels the panel shows, as L8. LVGL colours are mapped to the
// nearest of them in the flush callback.
#define UI_L8_WHITE 255
#define UI_L8_LIGHT 170
#define UI_L8_DARK 85
#define UI_L8_BLACK 0

#define UI_GRAY(l8) lv_color_make((l8), (l8), (l8))

// Focused widgets: black background, white text (the mono theme's outline
// gets clipped by containers).
extern lv_style_t ui_style_focus;

// The avatar as an image, in a buffer shared by all pages; NULL without one.
// rotate180: for the home page, which faces the person in front.
const lv_image_dsc_t *ui_avatar_image(const contact_t *c, bool rotate180);

// A blank white screen, and showing one (the old screen is deleted).
lv_obj_t *ui_screen_create(void);
void      ui_screen_show(lv_obj_t *scr);
