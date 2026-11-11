#include "board_spi.h"

#include "board_pins.h"

esp_err_t board_spi_init(void)
{
    const spi_bus_config_t config = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER,
    };
    return spi_bus_initialize(BOARD_SPI_HOST, &config, SPI_DMA_CH_AUTO);
}
