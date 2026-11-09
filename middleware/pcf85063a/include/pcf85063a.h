#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    uint16_t year;     // full year, e.g. 2026 (the chip stores year % 100)
    uint8_t  month;    // 1..12
    uint8_t  day;      // 1..31
    uint8_t  hours;    // 0..23
    uint8_t  minutes;  // 0..59
    uint8_t  seconds;  // 0..59
} pcf85063a_time_t;

// Control_2 interrupt flags, as returned by pcf85063a_read_flags().
#define PCF85063A_FLAG_AF 0x40  // alarm
#define PCF85063A_FLAG_TF 0x08  // countdown timer

// Attaches the chip to the I2C bus and disables CLKOUT. Leaves AF/TF alone:
// the RTC may be the reason the MCU just woke up.
esp_err_t pcf85063a_init(void);

// Sets the clock and marks it trusted. The seal (RAM byte) is cleared first
// and written last, so a write cut short leaves the time untrusted.
esp_err_t pcf85063a_set_time(const pcf85063a_time_t *t);

// Reads the clock. *trusted is true only if the oscillator never stopped and
// the last set_time completed. Fail-closed: false on any error.
esp_err_t pcf85063a_read_time(pcf85063a_time_t *t, bool *trusted);

// Countdown timer on the open-drain INT pin. INT stays low until
// pcf85063a_clear_flags(). Periodic: reloads and fires again. Up to 255 s at
// 1 Hz, beyond that rounded up to whole minutes (max 255 min, longer returns
// ESP_ERR_INVALID_ARG).
esp_err_t pcf85063a_start_timer(uint32_t seconds);
// Stops the timer and leaves it on the 1/60 Hz clock, which draws the least.
esp_err_t pcf85063a_stop_timer(void);

// AF/TF as currently set (PCF85063A_FLAG_*).
esp_err_t pcf85063a_read_flags(uint8_t *flags);
// Clears AF and TF, which releases INT.
esp_err_t pcf85063a_clear_flags(void);
