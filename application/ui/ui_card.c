// Contact card, as in the reference design: the avatar fills the left half;
// on the right a black header with the name, then species and home on light
// gray.

#include "ui.h"
#include "ui_priv.h"

#define CARD_HALF 128
#define HEADER_H 20
#define TEXT_X 4
#define TEXT_W (CARD_HALF - 2 * TEXT_X)

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
    lv_obj_t *scr = ui_screen_create();

    const lv_image_dsc_t *avatar = ui_avatar_image(c, false);
    if (avatar)
    {
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, avatar);
        lv_obj_set_pos(img, (CARD_HALF - c->avatar_res) / 2, (CARD_HALF - c->avatar_res) / 2);
    }

    lv_obj_t *header = box(scr, CARD_HALF, 0, CARD_HALF, HEADER_H, UI_L8_BLACK);
    line(header, (HEADER_H - lv_font_get_line_height(UI_FONT_LARGE)) / 2, UI_FONT_LARGE,
         UI_L8_WHITE, c->name);

    lv_obj_t *body = box(scr, CARD_HALF, HEADER_H, CARD_HALF, CARD_HALF - HEADER_H, UI_L8_LIGHT);
    const int32_t step = lv_font_get_line_height(UI_FONT_SMALL) + 2;
    line(body, 4, UI_FONT_SMALL, UI_L8_BLACK, c->species);
    line(body, 4 + step, UI_FONT_SMALL, UI_L8_BLACK, c->from);

    ui_screen_show(scr);
}
