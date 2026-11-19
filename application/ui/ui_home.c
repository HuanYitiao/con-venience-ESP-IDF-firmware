// Home page, as the old firmware: the own avatar in the middle, turned
// upside down for the person in front, between two dithered side bands
// that double as the battery gauge. Bands are pure black and white, so the
// avatar's grays take no crosstalk from them.

#include "battery.h"
#include "ui.h"
#include "ui_priv.h"

#define SCREEN_W 256
#define SCREEN_H 128
// Old firmware: 48 px bands at full battery, 5 px narrower per level below,
// and a 2 px solid line each side at level 0.
#define BAND_W_FULL 48
#define BAND_STEP 5
#define BAND_W_EMPTY 2

static uint8_t        band_l8[2][BAND_W_FULL * SCREEN_H];
static lv_image_dsc_t band_img[2];

// Bayer 4x4: a pixel is black where its threshold is below the density.
static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// Black at the outer edge, fading inward with density 1 - t^2 (t = col / w),
// the old firmware's formula. mirror: outer edge on the right.
static void make_band(int side, int w, bool mirror)
{
    uint8_t *px = band_l8[side];
    for (int col = 0; col < w; col++)
    {
        int density = (w == BAND_W_EMPTY) ? 16 : 16 - (16 * col * col + w * w / 2) / (w * w);
        int x = mirror ? w - 1 - col : col;
        for (int y = 0; y < SCREEN_H; y++)
        {
            px[y * w + x] = bayer4[y % 4][col % 4] < density ? UI_L8_BLACK : UI_L8_WHITE;
        }
    }
    lv_image_dsc_t *img = &band_img[side];
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_L8;
    img->header.w = w;
    img->header.h = SCREEN_H;
    img->header.stride = w;
    img->data_size = w * SCREEN_H;
    img->data = px;
}

void ui_show_home(const contact_t *self, uint8_t battery_level)
{
    lv_obj_t *scr = ui_screen_create();

    if (battery_level > BATTERY_LEVELS)
    {
        battery_level = BATTERY_LEVELS;
    }
    int w = battery_level == 0 ? BAND_W_EMPTY
                               : BAND_W_FULL - (BATTERY_LEVELS - battery_level) * BAND_STEP;
    make_band(0, w, false);
    make_band(1, w, true);
    lv_obj_t *left = lv_image_create(scr);
    lv_image_set_src(left, &band_img[0]);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_t *right = lv_image_create(scr);
    lv_image_set_src(right, &band_img[1]);
    lv_obj_set_pos(right, SCREEN_W - w, 0);

    const lv_image_dsc_t *avatar = ui_avatar_image(self, true);
    if (avatar)
    {
        lv_obj_t *img = lv_image_create(scr);
        lv_image_set_src(img, avatar);
        lv_obj_set_pos(img, (SCREEN_W - self->avatar_res) / 2, (SCREEN_H - self->avatar_res) / 2);
    }

    ui_screen_show(scr);
}
