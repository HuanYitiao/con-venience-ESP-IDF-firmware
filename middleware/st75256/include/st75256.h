#pragma once

#include <stdint.h>

#include "esp_err.h"

// JLX256128G-931: ST75256, 256x128, 4 gray levels.
//
// Data format is the controller's own: the screen is split into pages of 4
// rows. A buffer covers a window of w columns by n pages, page after page,
// one byte per column within a page. Each byte holds 4 vertically stacked
// pixels, 2 bits each, top pixel in bits 7..6.
#define ST75256_WIDTH 256
#define ST75256_HEIGHT 128
#define ST75256_PAGE_HEIGHT 4
#define ST75256_PAGES (ST75256_HEIGHT / ST75256_PAGE_HEIGHT)

// Gray levels (2 bits per pixel).
#define ST75256_WHITE 0x0
#define ST75256_LIGHT_GRAY 0x1
#define ST75256_DARK_GRAY 0x2
#define ST75256_BLACK 0x3

// Drive settings that trade contrast against crosstalk. Defaults are the old
// firmware's values; st75256_init() applies them.
typedef struct
{
    uint16_t vop;         // 0x81, 0..511: contrast
    uint8_t  bias;        // 0x32 P3, 0..5: 1/14 .. 1/9
    uint8_t  booster;     // 0x32 P2, 0..3: booster efficiency
    uint8_t  line_inv;    // 0xCA P3, raw: N-line inversion (low bits) and frame inversion
    uint8_t  frame_rate;  // 0xF0 (EXT1), 0..31; ST75256_FRAME_RATE_DEFAULT = not sent
} st75256_tuning_t;

#define ST75256_FRAME_RATE_DEFAULT 0xFF

// The defaults, or the values last applied.
st75256_tuning_t st75256_get_tuning(void);

// Sends the settings while the display runs. Clamps out-of-range values.
esp_err_t st75256_set_tuning(const st75256_tuning_t *t);

// Resets the controller (LCD_RST on MCP GPA3), sends the init sequence and
// clears the screen. Backlight stays off. Call after power_early_init() and
// board_spi_init(). The backlight also needs +3.3V_PERIPH.
esp_err_t st75256_init(void);

// Writes a window: columns x..x+w-1, pages page..page+pages-1, w * pages
// bytes in the format above. Blocks until sent. Not thread-safe: call from
// one task only.
esp_err_t st75256_write(int x, int page, int w, int pages, const uint8_t *data);

// 0..100.
esp_err_t st75256_set_backlight(uint8_t percent);

// Before power-off. The controller sits on the always-on rail: sleep it and
// hold it in reset, so the floating SPI lines of the powered-down MCU can't
// disturb it. st75256_init() wakes it again on the next boot.
esp_err_t st75256_sleep(void);
