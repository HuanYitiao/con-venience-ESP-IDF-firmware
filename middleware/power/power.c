#include "power.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "esp_log.h"
#include "mcp23017.h"
#include "pcf85063a.h"

static const char *TAG = "power";

// GPA3 (LCD_RST) is intentionally excluded: it is owned by the display layer.
#define POWER_PORTA_MASK                                                              \
    ((1 << PIN_MCP_BAT_MON_EN) | (1 << PIN_MCP_PWR_PERIPH_EN) | (1 << PIN_MCP_SPK_EN) \
     | (1 << PIN_MCP_SLEEP_REQ))

static power_wake_t wake_reason = POWER_WAKE_COLD_BOOT;

// Reads and clears the wake sources. Errors are only logged: the keep-alive
// is already done, and an unreadable flag just counts as not set.
static void detect_wake(bool was_asleep)
{
    uint8_t   btn_flags = 0;
    esp_err_t err = mcp23017_read_reg(MCP_INTFB, &btn_flags);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Read INTFB failed: %s", esp_err_to_name(err));
        btn_flags = 0;
    }
    // Reading INTF does not release INTB; that happens when GPIOB is read.
    btn_flags &= MCP_BTN_MASK;

    uint8_t rtc_flags = 0;
    err = pcf85063a_read_flags(&rtc_flags);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Read RTC flags failed: %s", esp_err_to_name(err));
        rtc_flags = 0;
    }

    // The timer is only armed while asleep. Left running, it would hold the
    // shared INT line low and swallow every later button edge.
    err = pcf85063a_stop_timer();
    if (err == ESP_OK)
    {
        err = pcf85063a_clear_flags();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Releasing the RTC failed: %s", esp_err_to_name(err));
    }

    if (!was_asleep)
    {
        wake_reason = POWER_WAKE_COLD_BOOT;
    }
    else if (btn_flags)
    {
        wake_reason = POWER_WAKE_BUTTON;
    }
    else if (rtc_flags)
    {
        wake_reason = POWER_WAKE_TIMER;
    }
    else
    {
        wake_reason = POWER_WAKE_OTHER;
    }
}

// On a wake from sleep SLEEP_REQ is still 1 and only /INT is holding MCU_EN
// high; clearing a wake flag before SLEEP_REQ drops would reset the MCU
// straight away. After a battery insert the MCP is fresh (OLATA = 0, all
// inputs): SLEEP_REQ floats and only U1 keeps the MCU up, for ~6 s.
esp_err_t power_early_init(void)
{
    // 1. Keep-alive. Read SLEEP_REQ before clearing it: it is the only record
    //    of having slept.
    uint8_t   olat = 0;
    esp_err_t err = mcp23017_read_reg(MCP_OLATA, &olat);
    if (err != ESP_OK)
    {
        return err;
    }
    bool was_asleep = (olat >> PIN_MCP_SLEEP_REQ) & 1;

    // Latch first, direction second: switching to output with the old latch
    // would briefly drive SLEEP_REQ high after a wake.
    err = mcp23017_update_reg(MCP_OLATA, POWER_PORTA_MASK, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    err = mcp23017_update_reg(MCP_IODIRA, POWER_PORTA_MASK, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    // 2. Now it is safe to look at (and release) the wake sources.
    detect_wake(was_asleep);
    return ESP_OK;
}

power_wake_t power_wake_reason(void)
{
    return wake_reason;
}

const char *power_wake_name(power_wake_t w)
{
    switch (w)
    {
        case POWER_WAKE_COLD_BOOT:
            return "COLD_BOOT";
        case POWER_WAKE_BUTTON:
            return "BUTTON";
        case POWER_WAKE_TIMER:
            return "TIMER";
        case POWER_WAKE_OTHER:
        default:
            return "OTHER";
    }
}
