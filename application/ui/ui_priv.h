#pragma once

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
