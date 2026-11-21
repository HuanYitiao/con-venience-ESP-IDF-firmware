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
#include "max98357a.h"
#include "mcp23017.h"
#include "nvs_flash.h"
#include "pairing.h"
#include "pcf85063a.h"
#include "power.h"
#include "sdcard.h"
#include "st75256.h"
#include "supervisor.h"
#include "ui.h"

static const char *TAG = "con-venience";

static void on_button(button_id_t id, button_action_t action)
{
    static const char *const names[BUTTON_COUNT] = {
        [BUTTON_UP] = "UP",       [BUTTON_DOWN] = "DOWN", [BUTTON_LEFT] = "LEFT",
        [BUTTON_RIGHT] = "RIGHT", [BUTTON_PAIR] = "PAIR",
    };
    ESP_LOGI(TAG, "Button %s %s", names[id], action == BUTTON_LONG_PRESS ? "long press" : "click");
    supervisor_activity();
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

// One VBAT reading per boot feeds the gauge. Needs NVS. Returns true if
// the battery is too low to run: checked at every boot, so a wake cannot
// hand out the full UI on a flat battery before the supervisor's first
// check (old firmware). A low reading is read again, up to three in all.
static bool battery_start(void)
{
    esp_err_t err = battery_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Battery init failed: %s", esp_err_to_name(err));
        return false;
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
        return false;
    }
    // On USB the reading may be of no battery at all (a few mV), and the
    // level never goes back up: keep it out of the gauge.
    if (power_usb_present())
    {
        ESP_LOGI(TAG, "VBAT %u mV on USB, level not updated (%u/%u)", mv, battery_level(),
                 BATTERY_LEVELS);
        return false;
    }
    battery_level_update(mv);
    boot_battery_mv = mv;
    ESP_LOGI(TAG, "VBAT %u mV, level %u/%u", mv, battery_level(), BATTERY_LEVELS);

    for (int i = 1; mv < BATTERY_LOW_MV; i++)
    {
        if (i == 3)
        {
            ESP_LOGW(TAG, "Battery low");
            return true;
        }
        if (battery_read_mv(&mv) != ESP_OK)
        {
            return false;
        }
        ESP_LOGI(TAG, "VBAT %u mV, again", mv);
    }
    return false;
}

// The old firmware's startup beep, on a cold boot only: a wake must stay
// silent. The amp is shut down again right after.
static void startup_beep(void)
{
    if (power_wake_reason() != POWER_WAKE_COLD_BOOT || max98357a_init() != ESP_OK)
    {
        return;
    }
    static const max98357a_note_t beep[] = {{2500, 50}};
    max98357a_set_waveform(MAX98357A_WAVE_SQUARE);
    max98357a_play(beep, 1, 15);
    max98357a_deinit();
}

// The reverse of the boot order: the display sleeps while the SPI bus is
// still up, the card lets go of the bus, then the peripheral rail goes.
static void shutdown(uint32_t wake_after_sec)
{
    st75256_sleep();
    if (sdcard_is_mounted())
    {
        sdcard_unmount();
    }
    power_periph_off();
    power_sleep(wake_after_sec);
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

    bool battery_low = false;
    err = nvs_init();
    if (err == ESP_OK)
    {
        battery_low = battery_start();
    }
    else
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

#if HW_TEST == HW_TEST_NONE
    // Woken by the RTC only to check the battery: if it is fine, straight
    // back to sleep, before the display, the card and BLE (old firmware).
    // A low battery boots on and shows the warning. On USB it boots fully.
    if (power_wake_reason() == POWER_WAKE_TIMER && !battery_low && !power_usb_present())
    {
        ESP_LOGI(TAG, "Timer wake, battery fine, back to sleep");
        power_sleep(FSM_WAKE_INTERVAL_S);
    }
#endif

    bool display_ok = display_start();
    if (display_ok)
    {
        startup_beep();  // needs +3.3V_PERIPH, which the display start switched on
    }

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
            bool             provisioning = false;
            if (sd_ok)
            {
                contacts_ensure_self_uuid(NULL, NULL);  // as the old firmware, at each boot
                contacts_list_friends();
                // As the old firmware: without a name and an avatar there is
                // nothing to show or send, so first-time setup comes first.
                err = contacts_load_self(&self);
                provisioning = err != ESP_OK || !self.name[0] || !self.avatar_res;
                if (provisioning)
                {
                    ESP_LOGW(TAG, "No usable own profile (%s), first-time setup",
                             err != ESP_OK ? esp_err_to_name(err) : "no name or avatar");
                }
            }
            // BLE and ACOM at boot, as the old firmware. Needs NVS.
            esp_err_t pair_err = pairing_init();
            if (pair_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Pairing init failed: %s", esp_err_to_name(pair_err));
            }
            const fsm_config_t config = {
                .self = &self,
                .battery_mv = boot_battery_mv,
                .sd_ok = sd_ok,
                .provisioning = provisioning,
                .battery_low = battery_low,
                .shutdown = shutdown,
            };
            fsm_start(&config);
            supervisor_start();
            ui_run();
        }
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
    }
#endif
}
