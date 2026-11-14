#include "sdcard.h"

#include <stdint.h>

#include "board_pins.h"
#include "board_spi.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp23017.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

// Same clock and retries as the old firmware (SD.begin at 4 MHz, 3 tries).
#define SDCARD_FREQ_KHZ 4000
#define SDCARD_MOUNT_TRIES 3
#define SDCARD_RETRY_DELAY_MS 500
// +3.3V_SD comes up behind a load switch after +3.3V_PERIPH.
#define SDCARD_SENSE_TIMEOUT_MS 50
#define SDCARD_MAX_OPEN_FILES 5

static sdmmc_card_t *card;

bool sdcard_present(void)
{
    // A card shows up as soon as the rail is up; only "no card" waits the full timeout.
    TickType_t t0 = xTaskGetTickCount();
    do
    {
        uint8_t gpiob = 0;
        if (mcp23017_read_reg(MCP_GPIOB, &gpiob) == ESP_OK && ((gpiob >> PIN_MCP_SD_SENSE) & 1))
        {
            return true;
        }
        vTaskDelay(1);
    } while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(SDCARD_SENSE_TIMEOUT_MS));
    return false;
}

esp_err_t sdcard_mount(void)
{
    if (card)
    {
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = SDCARD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SPI_HOST;
    slot.gpio_cs = PIN_SD_CS;
    // The driver polls MISO for "not busy" before asserting CS. On this board
    // CS also enables U17, so with CS high the card is cut off and MISO
    // carries nothing: skip that poll. Busy after a write is still polled
    // with CS low.
    slot.wait_for_miso = -1;

    const esp_vfs_fat_mount_config_t mount_config = {
        // Formatting would wipe an old card that just failed to mount once.
        .format_if_mount_failed = false,
        .max_files = SDCARD_MAX_OPEN_FILES,
    };

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < SDCARD_MOUNT_TRIES; i++)
    {
        err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_config, &card);
        if (err == ESP_OK)
        {
            break;
        }
        card = NULL;
        ESP_LOGW(TAG, "Mount try %d failed: %s", i + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(SDCARD_RETRY_DELAY_MS));
    }
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "Mounted %s, %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    if (!card)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // Also removes the SPI device and turns CS into a plain input.
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, card);
    card = NULL;
    return err;
}

bool sdcard_is_mounted(void)
{
    return card != NULL;
}
