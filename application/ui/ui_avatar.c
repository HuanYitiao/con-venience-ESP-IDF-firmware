// Avatars: from the panel's 2bpp layout to an L8 image for LVGL. Only one
// avatar is on screen at a time, so they share one buffer.

#include "ui_priv.h"

static uint8_t        avatar_l8[CONTACT_AVATAR_RES_MAX * CONTACT_AVATAR_RES_MAX];
static lv_image_dsc_t avatar_img;

static const uint8_t level_to_l8[4] = {UI_L8_WHITE, UI_L8_LIGHT, UI_L8_DARK, UI_L8_BLACK};

// 2bpp pages of 4 rows, one byte per column, top row in bits 7..6.
const lv_image_dsc_t *ui_avatar_image(const contact_t *c, bool rotate180)
{
    const int res = c->avatar_res;
    if (res == 0 || res > CONTACT_AVATAR_RES_MAX)
    {
        return NULL;
    }
    for (int page = 0; page < res / 4; page++)
    {
        for (int x = 0; x < res; x++)
        {
            uint8_t byte = c->avatar[page * res + x];
            for (int r = 0; r < 4; r++)
            {
                int y = page * 4 + r;
                int i = rotate180 ? (res - 1 - y) * res + (res - 1 - x) : y * res + x;
                avatar_l8[i] = level_to_l8[(byte >> ((3 - r) * 2)) & 3];
            }
        }
    }

    // The descriptor is reused. LVGL's image cache is off
    // (CONFIG_LV_CACHE_DEF_SIZE=0), so nothing holds on to the old pixels.
    avatar_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    avatar_img.header.cf = LV_COLOR_FORMAT_L8;
    avatar_img.header.w = res;
    avatar_img.header.h = res;
    avatar_img.header.stride = res;
    avatar_img.data_size = res * res;
    avatar_img.data = avatar_l8;
    return &avatar_img;
}

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, UI_GRAY(UI_L8_WHITE), 0);
    return scr;
}

void ui_screen_show(lv_obj_t *scr)
{
    // No transition: STN smears anything that moves.
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}
