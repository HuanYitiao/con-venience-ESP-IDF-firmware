#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "board_i2c.h"
#include "board_spi.h"
#include "button.h"
#include "contacts.h"
#include "esp_log.h"
#include "esp_system.h"
#include "fsm.h"
#include "hw_tests.h"
#include "mcp23017.h"
#include "nvs_flash.h"
#include "pcf85063a.h"
#include "power.h"
#include "sdcard.h"
#include "st75256.h"
#include "ui.h"

static const char *TAG = "con-venience";

static void on_button(button_id_t id, button_action_t action)
{
    static const char *const names[BUTTON_COUNT] = {
        [BUTTON_UP] = "UP",       [BUTTON_DOWN] = "DOWN", [BUTTON_LEFT] = "LEFT",
        [BUTTON_RIGHT] = "RIGHT", [BUTTON_PAIR] = "PAIR",
    };
    ESP_LOGI(TAG, "Button %s %s", names[id], action == BUTTON_LONG_PRESS ? "long press" : "click");
    hw_test_on_button(id, action);
    ui_on_button(id, action);
}

// Standard IDF init: a full or outdated NVS partition is erased and set up again.
static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            err = nvs_flash_init();
        }
    }
    return err;
}

// The boot reading for the menu; 0 without one (on USB, or a failed read).
static uint16_t boot_battery_mv;

// One VBAT reading per boot feeds the gauge. Needs NVS.
static void battery_start(void)
{
    esp_err_t err = battery_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Battery init failed: %s", esp_err_to_name(err));
        return;
    }

    // COLD_BOOT also covers esp_restart, panic and watchdog resets; only a
    // real power-up means the cells may have been swapped.
    bool powered_up = power_wake_reason() == POWER_WAKE_COLD_BOOT
                      && esp_reset_reason() == ESP_RST_POWERON;
    battery_level_init(powered_up);

    uint16_t mv = 0;
    err = battery_read_mv(&mv);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "VBAT read failed: %s", esp_err_to_name(err));
        return;
    }
    // On USB the reading may be of no battery at all (a few mV), and the
    // level never goes back up: keep it out of the gauge.
    if (power_usb_present())
    {
        ESP_LOGI(TAG, "VBAT %u mV on USB, level not updated (%u/%u)", mv, battery_level(),
                 BATTERY_LEVELS);
        return;
    }
    battery_level_update(mv);
    boot_battery_mv = mv;
    ESP_LOGI(TAG, "VBAT %u mV, level %u/%u", mv, battery_level(), BATTERY_LEVELS);
}

// +3.3V_PERIPH, then the SPI bus (driving it into an unpowered SD card would
// back-feed it), then the display, which also takes its CS high before any
// SD access. Returns false if the display is not usable.
static bool display_start(void)
{
    esp_err_t err = power_periph_on();
    if (err == ESP_OK)
    {
        err = board_spi_init();
    }
    if (err == ESP_OK)
    {
        err = st75256_init();
    }
    if (err == ESP_OK)
    {
        err = st75256_set_backlight(100);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Display start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Keep-alive comes first: after a wake only the wake INT, and after a
// battery insert only U1 (~6 s), holds the MCU up until SLEEP_REQ drops.
static void power_start(void)
{
    esp_err_t err = board_i2c_init();
    if (err == ESP_OK)
    {
        err = mcp23017_init();
    }
    // Before power_early_init, which reads the RTC flags. Safe this early:
    // init never clears a wake flag, so a waking INT keeps holding the power.
    esp_err_t rtc_err = pcf85063a_init();
    if (err == ESP_OK)
    {
        err = power_early_init();
    }
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Keep-alive set, wake: %s", power_wake_name(power_wake_reason()));
    }
    else
    {
        ESP_LOGE(TAG, "Keep-alive failed: %s", esp_err_to_name(err));
    }
    if (rtc_err != ESP_OK)
    {
        ESP_LOGE(TAG, "PCF85063A init failed: %s", esp_err_to_name(rtc_err));
    }
}

// Without a card the UI only shows the "No SD Card" page.
static void storage_start(void)
{
    if (!sdcard_present())
    {
        ESP_LOGW(TAG, "No SD card");
        return;
    }
    esp_err_t err = sdcard_mount();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SD card mounted");
}

// Boot order matters; see README, "Boot order". No ESP_ERROR_CHECK on this
// path: an I2C error must not turn into a reboot loop.
void app_main(void)
{
    power_start();

    // After power_early_init: reading GPIOB clears the wake flags.
    esp_err_t err = button_init(on_button);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(err));
    }

    err = nvs_init();
    if (err == ESP_OK)
    {
        battery_start();
    }
    else
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    bool display_ok = display_start();

#if HW_TEST != HW_TEST_NONE
    hw_test_run(display_ok);
#else
    if (display_ok)
    {
        storage_start();  // after the display: its CS must be high before SD traffic
        err = ui_init();
        if (err == ESP_OK)
        {
            static contact_t self;  // 4.8 KB, mostly the avatar
            bool             sd_ok = sdcard_is_mounted();
            if (sd_ok)
            {
                contacts_ensure_self_uuid(NULL, NULL);  // as the old firmware, at each boot
                contacts_list_friends();
                err = contacts_load_self(&self);
                if (err != ESP_OK)
                {
                    // First-time setup (A5) takes over here; until then a
                    // made-up profile keeps the pages usable.
                    ESP_LOGW(TAG, "No own profile (%s), using made-up data", esp_err_to_name(err));
                    contacts_fake(&self);
                }
            }
            const fsm_config_t config = {
                .self = &self,
                .battery_mv = boot_battery_mv,
                .sd_ok = sd_ok,
            };
            fsm_start(&config);
            ui_run();
        }
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
    }
#endif
}
