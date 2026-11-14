#include "board_spi.h"

#include "board_pins.h"
#include "driver/gpio.h"

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
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        return err;
    }
    // MISO only has a driver while the SD card is selected (U17 is off
    // otherwise); keep it from floating. U17's A side is on +3.3V_MCU, so
    // this does not reach the SD rail.
    return gpio_pullup_en(PIN_SPI_MISO);
}
