#include "pcf85063a.h"

#include "board_i2c.h"
#include "board_pins.h"

#define REG_CONTROL_1 0x00
#define REG_CONTROL_2 0x01
#define CTRL2_AIE 0x80
#define CTRL2_COF_OFF 0x07

static i2c_master_dev_handle_t dev;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, BOARD_I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), BOARD_I2C_TIMEOUT_MS);
}

esp_err_t pcf85063a_init(void)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063A_ADDR,
        .scl_speed_hz = BOARD_I2C_SCL_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &config, &dev);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t ctrl1 = 0;
    err = read_regs(REG_CONTROL_1, &ctrl1, 1);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t ctrl2 = 0;
    err = read_regs(REG_CONTROL_2, &ctrl2, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    return write_reg(REG_CONTROL_2,
                     (ctrl2 & CTRL2_AIE) | PCF85063A_FLAG_AF | PCF85063A_FLAG_TF | CTRL2_COF_OFF);
}