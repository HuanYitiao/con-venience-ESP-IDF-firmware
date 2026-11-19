// Contact card, as in the reference design: the avatar fills the left half;
// on the right a black header with the name, then species and home on light
// gray.

#include <string.h>

#include "ui.h"
#include "ui_priv.h"

#define CARD_HALF 128
#define HEADER_H 20
#define TEXT_X 4
#define TEXT_W (CARD_HALF - 2 * TEXT_X)

// The avatar as L8 for LVGL, converted from the panel's 2bpp layout.
static uint8_t        avatar_l8[CONTACT_AVATAR_RES_MAX * CONTACT_AVATAR_RES_MAX];
static lv_image_dsc_t avatar_img;

static const uint8_t level_to_l8[4] = {UI_L8_WHITE, UI_L8_LIGHT, UI_L8_DARK, UI_L8_BLACK};

// 2bpp pages of 4 rows, one byte per column, top row in bits 7..6.
static void avatar_to_l8(const contact_t *c)
{
    const int res = c->avatar_res;
    for (int page = 0; page < res / 4; page++)
    {
        for (int x = 0; x < res; x++)
        {
            uint8_t byte = c->avatar[page * res + x];
            for (int r = 0; r < 4; r++)
            {
                avatar_l8[(page * 4 + r) * res + x] = level_to_l8[(byte >> ((3 - r) * 2)) & 3];
            }
        }
    }

    // The descriptor is reused for every card. LVGL's image cache is off
    // (CONFIG_LV_CACHE_DEF_SIZE=0), so nothing holds on to the old pixels.
    avatar_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    avatar_img.header.cf = LV_COLOR_FORMAT_L8;
    avatar_img.header.w = res;
    avatar_img.header.h = res;
    avatar_img.header.stride = res;
    avatar_img.data_size = res * res;
    avatar_img.data = avatar_l8;
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t l8)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, UI_GRAY(l8), 0);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

// One line, cut with dots if it does not fit.
static lv_obj_t *line(lv_obj_t *parent, int32_t y, const lv_font_t *font, uint8_t l8,
                      const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, UI_GRAY(l8), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, TEXT_W);
    lv_obj_set_pos(label, TEXT_X, y);
    lv_label_set_text(label, text);
    return label;
}

void ui_show_contact_card(const contact_t *c)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, UI_GRAY(UI_L8_WHITE), 0);

    if (c->avatar_res)
    {
        avatar_to_l8(c);
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, &avatar_img);
        lv_obj_set_pos(img, (CARD_HALF - c->avatar_res) / 2, (CARD_HALF - c->avatar_res) / 2);
    }

    lv_obj_t *header = box(scr, CARD_HALF, 0, CARD_HALF, HEADER_H, UI_L8_BLACK);
    line(header, (HEADER_H - lv_font_get_line_height(UI_FONT_LARGE)) / 2, UI_FONT_LARGE,
         UI_L8_WHITE, c->name);

    lv_obj_t *body = box(scr, CARD_HALF, HEADER_H, CARD_HALF, CARD_HALF - HEADER_H, UI_L8_LIGHT);
    const int32_t step = lv_font_get_line_height(UI_FONT_SMALL) + 2;
    line(body, 4, UI_FONT_SMALL, UI_L8_BLACK, c->species);
    line(body, 4 + step, UI_FONT_SMALL, UI_L8_BLACK, c->from);

    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}
