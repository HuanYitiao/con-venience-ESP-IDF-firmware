#pragma once

// Pin map of the current board: an ESP32-C6 DevKit soldered directly onto the
// carrier. A move to a bare module is planned; re-check this file when it happens.

// ── SPI Bus ──────────────────────────────────────────────
#define PIN_SPI_SCK 11
#define PIN_SPI_MISO 10
#define PIN_SPI_MOSI 2

// ── Display (ST75256) ────────────────────────────────────
#define PIN_DISPLAY_CS 23
#define PIN_DISPLAY_DC 15
#define PIN_DISPLAY_BL 4

// ── MicroSD ──────────────────────────────────────────────
#define PIN_SD_CS 3

// ── WS2812B LED ──────────────────────────────────────────
#define PIN_LED 8
// The LED on the current DevKit takes RGB order, not the usual WS2812 GRB:
// with GRB, red and green swap (checked 2026-11-10). For led_strip.
#define PIN_LED_COLOR_FORMAT LED_STRIP_COLOR_COMPONENT_FMT_RGB

// ── I2C ──────────────────────────────────────────────────
#define PIN_I2C_SDA 6
#define PIN_I2C_SCL 7
#define PCF85063A_ADDR 0x51

// ── I2S Audio ────────────────────────────────────────────
#define PIN_I2S_BCLK 21
#define PIN_I2S_LRC 22
#define PIN_I2S_DIN 20

// ── ACOM ─────────────────────────────────────────────────
#define PIN_ACOM_OD 19
#define PIN_ACOM_PU 18

// ── ADC ──────────────────────────────────────────────────
#define PIN_VBAT_ADC 1

// ── MCP23017 ─────────────────────────────────────────────
#define PIN_MCP_INT 5
#define MCP23017_ADDR 0x20

// Buttons on Port B. SW1..SW5 = GPB2..GPB6 (SW6 is in parallel with SW5).
// GPB0 is the SD-rail sense input, GPB1 the USB power sense; neither is a button.
#define PIN_MCP_BTN_UP 5
#define PIN_MCP_BTN_DOWN 4
#define PIN_MCP_BTN_PAIR 6
#define PIN_MCP_BTN_LEFT 2
#define PIN_MCP_BTN_RIGHT 3
#define MCP_BTN_MASK                                                           \
    ((1 << PIN_MCP_BTN_UP) | (1 << PIN_MCP_BTN_DOWN) | (1 << PIN_MCP_BTN_PAIR) \
     | (1 << PIN_MCP_BTN_LEFT) | (1 << PIN_MCP_BTN_RIGHT))

// SD sense: +3.3V_SD via R7, high while a card is in (and +3.3V_PERIPH is on).
// No pull-up: an internal one would back-feed the SD rail.
#define PIN_MCP_SD_SENSE 0

// USB_PWR: high while powered from USB.
#define PIN_MCP_USB_PWR 1

// Outputs on Port A.
#define PIN_MCP_BAT_MON_EN 0
#define PIN_MCP_PWR_PERIPH_EN 1
#define PIN_MCP_SPK_EN 2
#define PIN_MCP_LCD_RST 3
#define PIN_MCP_SLEEP_REQ 4
