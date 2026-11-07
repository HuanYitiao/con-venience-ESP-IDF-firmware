#include "mcp23017.h"

#include <stdint.h>

#include "board_i2c.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;

// IOCON sits at 0x0A in BANK=0 and at MCP_IOCON (0x05) in BANK=1.
#define MCP_IOCON_BANK0 0x0A
#define MCP_IOCON_BANK 0x80
#define MCP_IOCON_ODR 0x04

static i2c_master_dev_handle_t dev;

// The MCP23017 sits on the always-on +3.3V_MCU rail, so after a sleep/wake
// cycle it is still in BANK=1 while after a battery insert it is in BANK=0.
// 0x05 is IOCON in BANK=1 (-> drop to BANK=0) and GPINTENB in BANK=0; 0x0A is
// IOCON in BANK=0, so the second write lands on IOCON either way.
// ODR stays set in the intermediate write: INT is wired-OR with the RTC's
// open-drain INT, and a push-pull INTB would fight it.
esp_err_t mcp23017_init(void)
{
    lock = xSemaphoreCreateMutexStatic(&lock_buf);

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MCP23017_ADDR,
        .scl_speed_hz = BOARD_I2C_SCL_HZ,
    };

    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &config, &dev);
    if (err != ESP_OK)
    {
        return err;
    }

    err = mcp23017_write_reg(MCP_IOCON, MCP_IOCON_ODR);
    if (err != ESP_OK)
    {
        return err;
    }

    return mcp23017_write_reg(MCP_IOCON_BANK0, MCP_IOCON_BANK | MCP_IOCON_ODR);
}

esp_err_t mcp23017_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, BOARD_I2C_TIMEOUT_MS);
}

esp_err_t mcp23017_write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), BOARD_I2C_TIMEOUT_MS);
}

esp_err_t mcp23017_update_reg(uint8_t reg, uint8_t mask, uint8_t val)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    uint8_t   old = 0;
    esp_err_t err = mcp23017_read_reg(reg, &old);
    if (err == ESP_OK)
    {
        err = mcp23017_write_reg(reg, (old & ~mask) | (val & mask));
    }

    xSemaphoreGive(lock);
    return err;
}
