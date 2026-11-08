#pragma once

#include <stdbool.h>

#include "esp_err.h"

// How the MCU is kept alive / switched off (DigiFur Rev1.0, sheet MCU):
//
//   MCU_EN = NAND(U1 RESET, /INT, MCU_SLEEP_REQ)   (U2 NL17SZ10 -> ESP32 EN)
//
//   - SLEEP_REQ (MCP GPA4) = 0 -> MCU runs.
//   - SLEEP_REQ = 1 and /INT high -> ESP32 held in reset (off).
//   - /INT pulled low (button or RTC) -> ESP32 cold-boots.
//   - U1 holds MCU_EN high for ~6 s after +3.3V_MCU comes up.

// Keep-alive. Call right after mcp23017_init(), before anything that touches
// a wake source. Drops SLEEP_REQ and puts every power output (Port A, except
// LCD_RST) into its off state.
esp_err_t power_early_init(void);

// SLEEP_REQ as found by power_early_init(): true = woke from powerSleep,
// false = cold boot. Basis for the wake reason in M2.
bool power_was_asleep(void);
