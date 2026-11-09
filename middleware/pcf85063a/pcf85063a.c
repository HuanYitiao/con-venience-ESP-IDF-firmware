#include "pcf85063a.h"

#include "board_i2c.h"
#include "board_pins.h"

#define REG_CONTROL_1 0x00
#define REG_CONTROL_2 0x01
#define CTRL2_AIE 0x80
#define CTRL2_COF_OFF 0x07

#define REG_RAM_BYTE 0x03
#define REG_SECONDS 0x04  // seconds..years at 0x04..0x0A

#define CTRL1_STOP 0x20
#define SECONDS_OS 0x80

#define TIME_VALID_MAGIC 0xA5
#define YEAR_BASE 2000

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
    // A set_time cut short leaves the clock stopped, and the timer with it.
    // Restart it; the seal is already cleared, so the time stays untrusted.
    if (ctrl1 & CTRL1_STOP)
    {
        err = write_reg(REG_CONTROL_1, (uint8_t)(ctrl1 & ~CTRL1_STOP));
        if (err != ESP_OK)
        {
            return err;
        }
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

static uint8_t bcd_to_dec(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t dec_to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

esp_err_t pcf85063a_set_time(const pcf85063a_time_t *t)
{
    esp_err_t err = write_reg(REG_RAM_BYTE, 0);
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
    err = write_reg(REG_CONTROL_1, (uint8_t)(ctrl1 | CTRL1_STOP));
    if (err != ESP_OK)
    {
        return err;
    }

    const uint8_t buf[] = {
        REG_SECONDS,
        dec_to_bcd(t->seconds),
        dec_to_bcd(t->minutes),
        dec_to_bcd(t->hours),
        dec_to_bcd(t->day),
        0,  // weekday (0x08): not used
        dec_to_bcd(t->month),
        dec_to_bcd(t->year % 100),
    };
    err = i2c_master_transmit(dev, buf, sizeof(buf), BOARD_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        return err;
    }

    err = write_reg(REG_CONTROL_1, (uint8_t)(ctrl1 & ~CTRL1_STOP));
    if (err != ESP_OK)
    {
        return err;
    }

    return write_reg(REG_RAM_BYTE, TIME_VALID_MAGIC);
}

esp_err_t pcf85063a_read_time(pcf85063a_time_t *t, bool *trusted)
{
    *trusted = false;
    uint8_t   b[7] = {0};
    esp_err_t err = read_regs(REG_SECONDS, b, sizeof(b));
    if (err != ESP_OK)
    {
        return err;
    }

    t->seconds = bcd_to_dec(b[0] & 0x7F);
    t->minutes = bcd_to_dec(b[1] & 0x7F);
    t->hours = bcd_to_dec(b[2] & 0x3F);
    t->day = bcd_to_dec(b[3] & 0x3F);  // 0x07 Days
    // b[4] = weekday (0x08) -- ignored on purpose
    t->month = bcd_to_dec(b[5] & 0x1F);  // 0x09 Months
    t->year = (uint16_t)(YEAR_BASE + bcd_to_dec(b[6]));

    uint8_t ram = 0;
    err = read_regs(REG_RAM_BYTE, &ram, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    *trusted = !(b[0] & SECONDS_OS) && ram == TIME_VALID_MAGIC;
    return ESP_OK;
}