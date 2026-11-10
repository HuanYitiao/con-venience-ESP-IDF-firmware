#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// VBAT -> R14/R15 (10k/10k) -> GPIO1 (ADC). The divider only draws current
// while BAT_MON_EN (MCP GPA0) is on; battery_read_mv() switches it.

// Gauge for 3x AA alkaline. Level 0 = nearly empty, BATTERY_LEVELS = full.
#define BATTERY_LEVELS 8

// Sets up the ADC and its calibration. Call once, after power_early_init()
// (it makes BAT_MON_EN an output).
esp_err_t battery_init(void);

// Switches the battery monitor on, averages a few samples, switches it off.
// Blocks for about 0.5 s while the divider settles.
esp_err_t battery_read_mv(uint16_t *mv);

// Loads the stored level, or forgets it when powered_up (cells may have been
// swapped). Call once per boot, after nvs_flash_init() and before the first
// battery_level_update().
void battery_level_init(bool powered_up);

// Feeds a reading into the gauge. The cells cannot recharge, so the level
// only goes down, and at most one step per reading. Skip readings taken on
// USB power.
void battery_level_update(uint16_t mv);

// 0..BATTERY_LEVELS. Full until the first reading of this battery.
uint8_t battery_level(void);
