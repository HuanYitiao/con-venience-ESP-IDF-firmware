#pragma once

#include "esp_err.h"

// How the MCU is kept alive / switched off (DigiFur Rev1.0, sheet MCU):
//
//   MCU_EN = NAND(U1 RESET, /INT, MCU_SLEEP_REQ)   (U2 NL17SZ10 -> ESP32 EN)
//
//   - SLEEP_REQ (MCP GPA4) = 0 -> MCU runs.
//   - SLEEP_REQ = 1 and /INT high -> ESP32 held in reset (off).
//   - /INT pulled low (button or RTC) -> ESP32 cold-boots.
//   - U1 holds MCU_EN high for ~6 s after +3.3V_MCU comes up.

typedef enum
{
    POWER_WAKE_COLD_BOOT,  // battery inserted, reset or restart (SLEEP_REQ was 0)
    POWER_WAKE_BUTTON,     // MCP23017 INTB
    POWER_WAKE_TIMER,      // PCF85063A countdown timer
    POWER_WAKE_OTHER,      // was asleep, but no wake flag found
} power_wake_t;

// Keep-alive, then wake reason. Call right after mcp23017_init() and
// pcf85063a_init() (neither clears a wake flag), before anything else that
// touches a wake source. Drops SLEEP_REQ and puts every power output (Port A,
// except LCD_RST) into its off state, then records and clears the wake flags.
// The return value only reports the keep-alive.
esp_err_t power_early_init(void);

// Wake reason found by power_early_init().
power_wake_t power_wake_reason(void);
const char  *power_wake_name(power_wake_t w);

// +3.3V_PERIPH: LCD backlight, speaker amp, and +3.3V_SD via the card-detect
// load switch. On waits for the rail to settle. Off also shuts the speaker
// amp down first (no pop). power_sleep() switches it off as well.
esp_err_t power_periph_on(void);
esp_err_t power_periph_off(void);

// Turns the MCU off. Wakes on a button press, and after wake_after_sec
// seconds if non-zero (RTC countdown). Peripherals must already be shut down.
// Does not return: if a wake source is already pending the MCU restarts
// instead, and the next boot reports that wake reason.
void power_sleep(uint32_t wake_after_sec) __attribute__((noreturn));
