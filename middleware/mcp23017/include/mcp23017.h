#pragma once

#include <stdint.h>

#include "esp_err.h"

// Register map with IOCON.BANK = 1 (set by mcp23017_init). Port A at
// 0x00..0x0A, port B at 0x10..0x1A.
#define MCP_IODIRA 0x00
#define MCP_GPINTENA 0x02
#define MCP_IOCON 0x05
#define MCP_INTFA 0x07
#define MCP_GPIOA 0x09
#define MCP_OLATA 0x0A

#define MCP_IODIRB 0x10
#define MCP_GPINTENB 0x12
#define MCP_DEFVALB 0x13
#define MCP_INTCONB 0x14
#define MCP_GPPUB 0x16
#define MCP_INTFB 0x17
#define MCP_INTCAPB 0x18
#define MCP_GPIOB 0x19

// Attaches the chip to the I2C bus and sets BANK=1. Call after
// board_i2c_init(). Leaves OLATA alone, so SLEEP_REQ survives a wake.
esp_err_t mcp23017_init(void);

esp_err_t mcp23017_read_reg(uint8_t reg, uint8_t *val);
esp_err_t mcp23017_write_reg(uint8_t reg, uint8_t val);

// Atomic read-modify-write: bits set in mask take their value from val, the
// others stay unchanged. Use this for registers shared between modules.
esp_err_t mcp23017_update_reg(uint8_t reg, uint8_t mask, uint8_t val);
