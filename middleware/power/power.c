#include "power.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp23017.h"
#include "pcf85063a.h"

static const char *TAG = "power";

// GPA3 (LCD_RST) is intentionally excluded: it is owned by the display layer.
#define POWER_PORTA_MASK                                                              \
    ((1 << PIN_MCP_BAT_MON_EN) | (1 << PIN_MCP_PWR_PERIPH_EN) | (1 << PIN_MCP_SPK_EN) \
     | (1 << PIN_MCP_SLEEP_REQ))

// Longest a button may still be held when power_sleep() is called.
#define POWER_BTN_RELEASE_TIMEOUT_MS 2000
// U1 (TLV840, CT=10u) holds MCU_EN high for ~6 s after +3.3V_MCU comes up.
// Wait a bit longer than that before giving up on the MCU switching off.
#define POWER_U1_HOLD_MAX_MS 8000
// Time for the USB serial port to send the last log line.
#define POWER_LOG_DRAIN_MS 20

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

static void log_if_failed(esp_err_t err, const char *what)
{
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
}

// Errors before the final SLEEP_REQ write are only logged: going to sleep
// with a missing wake source is better than staying stuck awake.
void power_sleep(uint32_t wake_after_sec)
{
    // 1. Let go of the button first, so the release can't count as a wake.
    //    Relies on button_init() having set the Port B pull-ups.
    TickType_t t0 = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(POWER_BTN_RELEASE_TIMEOUT_MS))
    {
        uint8_t gpiob = 0;
        if (mcp23017_read_reg(MCP_GPIOB, &gpiob) == ESP_OK
            && (gpiob & MCP_BTN_MASK) == MCP_BTN_MASK)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 2. Buttons: interrupt only while a pin differs from DEFVAL (= pressed).
    //    The button module switches back to interrupt-on-change on the next
    //    boot. Reading GPIOB clears any interrupt still pending.
    log_if_failed(mcp23017_update_reg(MCP_DEFVALB, MCP_BTN_MASK, MCP_BTN_MASK), "DEFVALB");
    log_if_failed(mcp23017_update_reg(MCP_INTCONB, MCP_BTN_MASK, MCP_BTN_MASK), "INTCONB");
    log_if_failed(mcp23017_update_reg(MCP_GPINTENB, MCP_BTN_MASK, MCP_BTN_MASK), "GPINTENB");
    uint8_t gpiob = 0;
    log_if_failed(mcp23017_read_reg(MCP_GPIOB, &gpiob), "GPIOB");

    // 3. RTC.
    if (wake_after_sec > 0)
    {
        log_if_failed(pcf85063a_start_timer(wake_after_sec), "RTC timer start");
    }
    else
    {
        log_if_failed(pcf85063a_stop_timer(), "RTC timer stop");
        log_if_failed(pcf85063a_clear_flags(), "RTC flag clear");
    }

    // 4. Everything off, then pull the plug in a separate write so the rails
    //    are already down when MCU_EN drops.
    const uint8_t rails =
        (1 << PIN_MCP_BAT_MON_EN) | (1 << PIN_MCP_PWR_PERIPH_EN) | (1 << PIN_MCP_SPK_EN);
    log_if_failed(mcp23017_update_reg(MCP_OLATA, rails, 0), "Rails off");

    ESP_LOGI(TAG, "Sleep, timer=%lu s", (unsigned long)wake_after_sec);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(POWER_LOG_DRAIN_MS));

    const uint8_t sleep_req = 1 << PIN_MCP_SLEEP_REQ;
    mcp23017_update_reg(MCP_OLATA, sleep_req, sleep_req);

    // 5. Normally the MCU is gone by now. Still running means either
    //    - INT is low: a wake source is pending -> restart with SLEEP_REQ
    //      still set, so the next power_early_init() reports that wake reason;
    //    - INT is high: U1 is still in its power-up window -> wait for it to
    //      release MCU_EN, which switches the MCU off.
    //    Keep this fallback as it is: wake reason detection depends on it.
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_MCP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    t0 = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < pdMS_TO_TICKS(POWER_U1_HOLD_MAX_MS))
    {
        if (gpio_get_level(PIN_MCP_INT) == 0)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    esp_restart();
}
