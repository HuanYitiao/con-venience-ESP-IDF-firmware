#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BOARD_I2C_SCL_HZ (100 * 1000)

// Upper bound for one transaction. A register access takes well under 1 ms at
// 100 kHz, so this only ever expires when the bus is stuck.
#define BOARD_I2C_TIMEOUT_MS 50

// Creates the shared I2C bus. Call once, first thing in app_main. The caller
// decides what a failure means: this runs on the keep-alive path, so it must
// not abort.
esp_err_t board_i2c_init(void);

// The bus handle for drivers to attach their devices to. NULL until
// board_i2c_init() has succeeded.
i2c_master_bus_handle_t board_i2c_bus(void);

// Probes every 7-bit address and logs each device found. Diagnostic only, not
// for the boot path. Stores the device count in *found, and returns an error
// if the bus itself fails.
esp_err_t board_i2c_scan(uint8_t *found);
