// A stand-in for the page flow (fsm), enough to walk every page with made-up
// data: home, RIGHT to the menu, and from there to every entry. LEFT goes
// back. Remove once the fsm exists.

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "ui.h"
#include "ui_priv.h"

static const char *TAG = "ui_demo";

// Pairing: each step after a fixed time, as if the peer were there.
#define PAIR_CONTACT_MS 2000
#define PAIR_CONNECT_MS 2000
#define PAIR_NO_TOTAL_MS 1000
#define PAIR_STEP_MS 300
#define PAIR_STEP_PCT 7

static const ui_demo_t *demo;
static bool             auto_sleep = true;
static int              wifi_step;
static lv_timer_t      *clock_timer;
static lv_timer_t      *pair_timer;
static int              pair_stage;  // 0 wait, 1 connecting, 2 transfer
static int              pair_pct;

static void show_home(void);
static void show_menu(void);

// The clock and pairing timers belong to their pages.
static void stop_timers(void)
{
    if (clock_timer)
    {
        lv_timer_delete(clock_timer);
        clock_timer = NULL;
    }
    if (pair_timer)
    {
        lv_timer_delete(pair_timer);
        pair_timer = NULL;
    }
}

// LEFT or PAIR long press from the peer's card back home.
static void card_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT || (id == BUTTON_PAIR && action == BUTTON_LONG_PRESS))
    {
        show_home();
    }
}

// LEFT back to the menu, for pages that only show something.
static void back_to_menu_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT)
    {
        show_menu();
    }
}

static void on_profile(void)
{
    ui_show_contact_card(demo->self);
    ui_set_key_handler(back_to_menu_keys);
}

static void on_friends(void)
{
    ui_show_friend_list(demo->friend_names, demo->friend_count, demo->load_friend, show_menu);
}

// Made-up AP details; the real ones come from wifi_ap.
static void show_wifi_step(void)
{
    ui_wifi_setup_t s = {
        .step = wifi_step,
        .wifi_qr = "WIFI:T:WPA;S:con-venience-3E55;P:0123456789ABCDEF;;",
        .url = "http://192.168.4.1/",
        .self_id = demo->self->uuid,
    };
    ui_show_wifi_setup(&s);
}

// UP/DOWN step through the three setup pages, to see each.
static void wifi_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT)
    {
        show_menu();
        return;
    }
    if (id == BUTTON_UP && wifi_step > 1)
    {
        wifi_step--;
    }
    else if (id == BUTTON_DOWN && wifi_step < 3)
    {
        wifi_step++;
    }
    else
    {
        return;
    }
    show_wifi_step();
    ui_set_key_handler(wifi_keys);
}

static void on_wifi(void)
{
    wifi_step = 1;
    show_wifi_step();
    ui_set_key_handler(wifi_keys);
}

// A made-up time that ticks from 2026-11-20 12:00:00 at boot.
static ui_clock_t fake_time(void)
{
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    return (ui_clock_t){
        .status = UI_CLOCK_OK,
        .year = 2026,
        .month = 11,
        .day = 20,
        .hours = (12 + s / 3600) % 24,
        .minutes = (s / 60) % 60,
        .seconds = s % 60,
    };
}

static void clock_tick(lv_timer_t *t)
{
    ui_clock_t c = fake_time();
    ui_clock_update(&c);
}

// UP/DOWN show the two failure states.
static void clock_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_LEFT)
    {
        show_menu();
        return;
    }
    ui_clock_t c = fake_time();
    if (id == BUTTON_UP)
    {
        c.status = UI_CLOCK_NOT_SET;
    }
    else if (id == BUTTON_DOWN)
    {
        c.status = UI_CLOCK_RTC_ERROR;
    }
    ui_show_clock(&c);
    ui_set_key_handler(clock_keys);
}

static void on_clock(void)
{
    ui_clock_t c = fake_time();
    ui_show_clock(&c);
    ui_set_key_handler(clock_keys);
    clock_timer = lv_timer_create(clock_tick, 1000, NULL);
}

static void on_auto_sleep(bool on)
{
    auto_sleep = on;
    ESP_LOGI(TAG, "Auto sleep %s", on ? "on" : "off");
}

static void on_reset(void)
{
    ui_show_message("Profile reset", "Rebooting...");
    ui_set_key_handler(back_to_menu_keys);  // the real one reboots
}

static const ui_menu_actions_t menu_actions = {
    .profile = on_profile,
    .friends = on_friends,
    .wifi_setup = on_wifi,
    .clock = on_clock,
    .auto_sleep = on_auto_sleep,
    .reset_profile = on_reset,
    .exit = show_home,
};

static void show_menu(void)
{
    stop_timers();
    ui_show_menu(&menu_actions, demo->battery_mv, auto_sleep);
}

// The peer's card after a pairing: the first made-up friend.
static void pair_done(void)
{
    stop_timers();
    static contact_t peer;  // 4.8 KB: not on the stack
    if (demo->load_friend(0, &peer) != ESP_OK)
    {
        contacts_fake(&peer);
    }
    ui_show_contact_card(&peer);
    ui_set_key_handler(card_keys);
}

static void pair_tick(lv_timer_t *t)
{
    switch (pair_stage)
    {
        case 0:  // "contact": connecting
            pair_stage = 1;
            ui_show_pairing_connecting(demo->self);
            lv_timer_set_period(t, PAIR_CONNECT_MS);
            break;
        case 1:  // connected: no total known at first
            pair_stage = 2;
            pair_pct = -1;
            ui_show_pairing_transfer(pair_pct);
            lv_timer_set_period(t, PAIR_NO_TOTAL_MS);
            break;
        default:
            pair_pct = pair_pct < 0 ? 0 : pair_pct + PAIR_STEP_PCT;
            if (pair_pct > 100)
            {
                pair_done();
                return;
            }
            ui_pairing_transfer_update(pair_pct);
            lv_timer_set_period(t, PAIR_STEP_MS);
            break;
    }
}

// PAIR long press cancels while waiting, as the old firmware.
static void pair_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_PAIR && action == BUTTON_LONG_PRESS && pair_stage == 0)
    {
        show_home();
    }
}

static void start_pairing(void)
{
    stop_timers();
    pair_stage = 0;
    ui_show_pairing_wait(demo->self);
    pair_timer = lv_timer_create(pair_tick, PAIR_CONTACT_MS, NULL);
    // Only waiting can be cancelled; connecting and the transfer take no
    // keys, as the old firmware (each ui_show_* drops the handler).
    ui_set_key_handler(pair_keys);
}

static void home_keys(button_id_t id, button_action_t action)
{
    if (id == BUTTON_RIGHT)
    {
        ui_menu_rewind();
        show_menu();
    }
    else if (id == BUTTON_PAIR && action == BUTTON_LONG_PRESS)
    {
        start_pairing();
    }
}

static void show_home(void)
{
    stop_timers();
    ui_show_home(demo->self, demo->battery_level);
    ui_set_key_handler(home_keys);
}

void ui_demo_start(const ui_demo_t *d)
{
    demo = d;
    show_home();
}
