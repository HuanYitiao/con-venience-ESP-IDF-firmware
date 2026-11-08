#include "power.h"

#include <stdint.h>

#include "board_pins.h"
#include "mcp23017.h"

// GPA3 (LCD_RST) is intentionally excluded: it is owned by the display layer.
#define POWER_PORTA_MASK                                                              \
    ((1 << PIN_MCP_BAT_MON_EN) | (1 << PIN_MCP_PWR_PERIPH_EN) | (1 << PIN_MCP_SPK_EN) \
     | (1 << PIN_MCP_SLEEP_REQ))

static bool was_asleep;

// On a wake from sleep SLEEP_REQ is still 1 and only /INT is holding MCU_EN
// high; clearing a wake flag before SLEEP_REQ drops would reset the MCU
// straight away. After a battery insert the MCP is fresh (OLATA = 0, all
// inputs): SLEEP_REQ floats and only U1 keeps the MCU up, for ~6 s.
esp_err_t power_early_init(void)
{
    // Read SLEEP_REQ before clearing it: it is the only record of having slept.
    uint8_t   olat = 0;
    esp_err_t err = mcp23017_read_reg(MCP_OLATA, &olat);
    if (err != ESP_OK)
    {
        return err;
    }
    was_asleep = (olat >> PIN_MCP_SLEEP_REQ) & 1;

    // Latch first, direction second: switching to output with the old latch
    // would briefly drive SLEEP_REQ high after a wake.
    err = mcp23017_update_reg(MCP_OLATA, POWER_PORTA_MASK, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    return mcp23017_update_reg(MCP_IODIRA, POWER_PORTA_MASK, 0);
}

bool power_was_asleep(void)
{
    return was_asleep;
}
