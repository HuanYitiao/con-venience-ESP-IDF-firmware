#pragma once

#include <stdbool.h>

#include "esp_err.h"

// FAT on the SD card, through the shared SPI bus. Files are reached with the
// normal C file API under this path, e.g. "/sdcard/self_profile/profile.json".
#define SDCARD_MOUNT_POINT "/sdcard"

// Card detect (MCP GPB0). Only meaningful with +3.3V_PERIPH on: waits up to
// 50 ms for the SD rail, so "no card" takes the full wait.
bool sdcard_present(void);

// Mounts the card, up to 3 tries. Never formats: a card that does not mount
// is left alone. Needs +3.3V_PERIPH, board_spi_init() and, so the display's
// CS is already high, st75256_init().
esp_err_t sdcard_mount(void);

// Before power_periph_off(). Leaves SD CS as a floating input: R66 then pulls
// it high, which disables the U17 level shifter. The SPI bus stays up for the
// display.
esp_err_t sdcard_unmount(void);

bool sdcard_is_mounted(void);
