// Housekeeping, as the old firmware's loop(): idle shutdown, the battery,
// USB power and card insertion. One task, every POLL_MS; a battery reading
// blocks for about 0.5 s, which the UI task must not.

#include "supervisor.h"

#include <stdbool.h>
#include <stdint.h>

#include "battery.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fsm.h"
#include "power.h"
#include "sdcard.h"
#include "wifi_ap.h"

static const char *TAG = "supervisor";

#define POLL_MS 500
// Battery: as the old firmware. The first check waits for the cells to
// recover from the boot load; three low readings in a row count.
#define BATTERY_SETTLE_MS 10000
#define BATTERY_INTERVAL_MS 30000
#define BATTERY_LOW_COUNT 3
// A card going in: let it seat before rebooting into it.
#define SD_SETTLE_MS 500
#define TASK_STACK 4096
#define TASK_PRIO 3

static volatile int64_t last_activity_us;

void supervisor_activity(void)
{
    last_activity_us = esp_timer_get_time();
}

static uint32_t ms_since(int64_t us)
{
    return (uint32_t)((esp_timer_get_time() - us) / 1000);
}

// Idle shutdown. The fsm says how long its page may sit idle (0: never).
// Held on USB or with auto sleep off (demo mode), the timer does not run.
static void check_idle(bool usb)
{
    static fsm_state_t last_state;
    static bool        posted;
    fsm_state_t        state = fsm_state();
    if (state != last_state || usb || !fsm_auto_sleep())
    {
        last_state = state;
        posted = false;
        supervisor_activity();
        return;
    }
    uint32_t limit = fsm_idle_limit_ms();
    uint32_t idle = ms_since(last_activity_us);
    if (state == FSM_WIFI_SETUP && wifi_ap_idle_ms() < idle)
    {
        idle = wifi_ap_idle_ms();  // the phone using the portal counts too
    }
    if (limit && idle > limit && !posted)
    {
        ESP_LOGI(TAG, "Idle for %lu s", (unsigned long)(idle / 1000));
        posted = true;  // once per state; the next state starts over
        fsm_post(FSM_EV_IDLE, 0);
    }
}

// On USB the reading may be of no battery at all: skipped, as at boot.
static void check_battery(bool usb)
{
    static int64_t last_us;
    static int     low_count;
    if (usb)
    {
        low_count = 0;
        return;
    }
    if (esp_timer_get_time() < (int64_t)BATTERY_SETTLE_MS * 1000
        || (last_us && ms_since(last_us) < BATTERY_INTERVAL_MS))
    {
        return;
    }
    last_us = esp_timer_get_time();
    uint16_t mv = 0;
    if (battery_read_mv(&mv) != ESP_OK)
    {
        return;
    }
    battery_level_update(mv);
    low_count = mv < BATTERY_LOW_MV ? low_count + 1 : 0;
    ESP_LOGI(TAG, "VBAT %u mV, level %u/%u%s", mv, battery_level(), BATTERY_LEVELS,
             low_count ? ", low" : "");
    if (low_count >= BATTERY_LOW_COUNT)
    {
        // Again on every check: a page that ignores it (pairing) takes the
        // next one once it is done.
        fsm_post(FSM_EV_BATTERY_LOW, 0);
    }
}

// "No SD Card": a card going in (an edge, so a card that does not mount
// cannot loop) reboots into the normal boot path, as the old firmware.
static void check_card(void)
{
    static bool was_present = true;  // the boot found none: the first poll sets it
    static bool first = true;
    if (fsm_state() != FSM_NO_SD)
    {
        first = true;
        return;
    }
    bool present = sdcard_present();
    if (present && !was_present && !first)
    {
        ESP_LOGI(TAG, "Card inserted, rebooting");
        vTaskDelay(pdMS_TO_TICKS(SD_SETTLE_MS));
        esp_restart();
    }
    was_present = present;
    first = false;
}

static void supervisor_task(void *arg)
{
    bool was_usb = !power_usb_present();  // logs the state on the first pass
    while (true)
    {
        bool usb = power_usb_present();
        if (usb != was_usb)
        {
            ESP_LOGI(TAG, "USB %s", usb ? "power: no sleep, no battery checks" : "removed");
            was_usb = usb;
        }
        check_idle(usb);
        check_battery(usb);
        check_card();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void supervisor_start(void)
{
    supervisor_activity();
    if (xTaskCreate(supervisor_task, "supervisor", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Task start failed");
    }
}
