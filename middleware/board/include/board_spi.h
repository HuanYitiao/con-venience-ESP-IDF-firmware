#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"

// Shared by the display and the SD card.
#define BOARD_SPI_HOST SPI2_HOST

// Largest single transfer: one full 2bpp frame of the 256x128 display.
#define BOARD_SPI_MAX_TRANSFER (256 * 128 / 4)

// Creates the shared SPI bus. Call once, with +3.3V_PERIPH on: driving the
// bus into an unpowered SD card would back-feed it.
esp_err_t board_spi_init(void);
