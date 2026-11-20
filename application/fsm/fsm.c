// The page flow, as the old firmware's fsm minus what LVGL now does (moving
// the focus in menus and lists). Every page change goes through enter():
// it leaves the old state (AP down, timers off), shows the new page and
// sets its keys. Pages with focus report choices through their callbacks;
// pages without take the buttons in page_keys().
//
// Pairing (ACOM, BLE) only has its pages here: the events that move it on
// come from A4.

#include "fsm.h"

#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pcf85063a.h"
#include "ui.h"
#include "wifi_ap.h"

static const char *TAG = "fsm";

// Times as the old firmware.
#define PEER_CARD_MS 10000
#define PAIRING_MS 30000
#define CONNECTING_MS 10000
#define DELETED_MS 800
#define DELETE_FAILED_MS 2000
#define RESET_MS 1000
#define RESET_FAILED_MS 3000
#define TICK_US (1000 * 1000)
#define QUEUE_LEN 16

static const char *const state_names[] = {
    [FSM_HOME] = "HOME",
    [FSM_MENU] = "MENU",
    [FSM_PROFILE] = "PROFILE",
    [FSM_LINKS] = "LINKS",
    [FSM_LINK_QR] = "LINK_QR",
    [FSM_FRIENDS] = "FRIENDS",
    [FSM_DELETE_CONFIRM] = "DELETE_CONFIRM",
    [FSM_DELETED] = "DELETED",
    [FSM_WIFI_SETUP] = "WIFI_SETUP",
    [FSM_CLOCK] = "CLOCK",
    [FSM_RESETTING] = "RESETTING",
    [FSM_PAIRING] = "PAIRING",
    [FSM_CONNECTING] = "CONNECTING",
    [FSM_TRANSFER] = "TRANSFER",
    [FSM_PEER_CARD] = "PEER_CARD",
    [FSM_NO_SD] = "NO_SD",
};

typedef struct
{
    fsm_event_t event;
    int         arg;
} queued_t;

static fsm_config_t  cfg;
static QueueHandle_t queue;
static fsm_state_t   state;
static bool          auto_sleep = true;

// The state's timeout: one-shot, tagged with a generation so a timeout that
// was already queued when the state changed is dropped.
static esp_timer_handle_t state_timer;
static int                generation;
static esp_timer_handle_t tick_timer;

static bool home_qr;  // home shows the own first link as a QR code

// The contact on the profile pages, and where LEFT on its card goes.
static const contact_t *profile;
static fsm_state_t      profile_back;
static int              link_focus;

static contact_t   friend_contact;  // 4.8 KB: not on the stack
static int         friend_focus;
static const char *friend_names[CONTACTS_FRIENDS_MAX];

static int wifi_step;  // 1 join the AP, 2 open the page

static void enter(fsm_state_t next, int arg);

// ── Timers ───────────────────────────────────────────────

static void on_state_timer(void *arg)
{
    fsm_post(FSM_EV_TIMEOUT, (int)(intptr_t)arg);
}

static void on_tick(void *arg)
{
    fsm_post(FSM_EV_TICK, 0);
}

static void start_timeout(uint32_t ms)
{
    // The generation travels as the timer's argument: recreate it per state.
    if (state_timer)
    {
        esp_timer_stop(state_timer);
        esp_timer_delete(state_timer);
        state_timer = NULL;
    }
    const esp_timer_create_args_t args = {
        .callback = on_state_timer,
        .arg = (void *)(intptr_t)generation,
        .name = "fsm_state",
    };
    if (esp_timer_create(&args, &state_timer) == ESP_OK)
    {
        esp_timer_start_once(state_timer, (uint64_t)ms * 1000);
    }
}

static void stop_timers(void)
{
    generation++;
    if (state_timer)
    {
        esp_timer_stop(state_timer);
    }
    esp_timer_stop(tick_timer);
}

// ── Pages ────────────────────────────────────────────────

static void show_clock(bool update)
{
    pcf85063a_time_t t;
    bool             trusted = false;
    ui_clock_t       c = {.status = UI_CLOCK_RTC_ERROR};
    if (pcf85063a_read_time(&t, &trusted) == ESP_OK)
    {
        c.status = trusted ? UI_CLOCK_OK : UI_CLOCK_NOT_SET;
        c.year = t.year;
        c.month = t.month;
        c.day = t.day;
        c.hours = t.hours;
        c.minutes = t.minutes;
        c.seconds = t.seconds;
    }
    if (update)
    {
        ui_clock_update(&c);
    }
    else
    {
        ui_show_clock(&c);
    }
}

static void show_wifi_step(void)
{
    static char qr[96];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", wifi_ap_ssid(), wifi_ap_password());
    const ui_wifi_setup_t s = {
        .step = wifi_step,
        .wifi_qr = qr,
        .url = wifi_ap_url(),
        .self_id = cfg.self->uuid,
    };
    ui_show_wifi_setup(&s);
}

// ── Callbacks of pages with focus (LVGL task) ────────────

static void menu_profile(void)
{
    profile = cfg.self;
    profile_back = FSM_MENU;
    enter(FSM_PROFILE, 0);
}

static void menu_friends(void)
{
    friend_focus = 0;
    enter(FSM_FRIENDS, 0);
}

static void menu_wifi(void)
{
    enter(FSM_WIFI_SETUP, 0);
}

static void menu_clock(void)
{
    enter(FSM_CLOCK, 0);
}

static void menu_auto_sleep(bool on)
{
    auto_sleep = on;
    ESP_LOGI(TAG, "Auto sleep %s", on ? "on" : "off");
}

static void menu_reset(void)
{
    enter(FSM_RESETTING, 0);
}

static void menu_exit(void)
{
    enter(FSM_HOME, 0);
}

static const ui_menu_actions_t menu_actions = {
    .profile = menu_profile,
    .friends = menu_friends,
    .wifi_setup = menu_wifi,
    .clock = menu_clock,
    .auto_sleep = menu_auto_sleep,
    .reset_profile = menu_reset,
    .exit = menu_exit,
};

// A failed load still opens the card, empty, rather than an older friend's.
static void load_friend(int index)
{
    if (contacts_load_friend(index, &friend_contact) != ESP_OK)
    {
        ESP_LOGW(TAG, "Friend %d does not load", index);
        memset(&friend_contact, 0, sizeof(friend_contact));
    }
}

static void friend_open(int index)
{
    friend_focus = index;
    load_friend(index);
    profile = &friend_contact;
    profile_back = FSM_FRIENDS;
    enter(FSM_PROFILE, 0);
}

static void friend_remove(int index)
{
    friend_focus = index;
    load_friend(index);
    enter(FSM_DELETE_CONFIRM, 0);
}

static void back_to_menu(void)
{
    enter(FSM_MENU, 0);
}

static const ui_friend_actions_t friend_actions = {
    .open = friend_open,
    .remove = friend_remove,
    .back = back_to_menu,
};

static void delete_cancel(void)
{
    enter(FSM_FRIENDS, 0);
}

static void delete_confirm(void)
{
    enter(FSM_DELETED, 0);
}

static void link_open(int index)
{
    link_focus = index;
    enter(FSM_LINK_QR, 0);
}

static void links_back(void)
{
    enter(FSM_PROFILE, 0);
}

// ── Buttons on pages without focus (LVGL task) ───────────

static void page_keys(button_id_t id, button_action_t action)
{
    const bool click = action == BUTTON_CLICK;
    const bool pair_long = id == BUTTON_PAIR && !click;
    const bool left = id == BUTTON_LEFT && click;
    const bool right = id == BUTTON_RIGHT && click;

    switch (state)
    {
        case FSM_HOME:
            if (right)
            {
                ui_menu_rewind();  // the menu opens on its first entry from home
                enter(FSM_MENU, 0);
            }
            else if (pair_long)
            {
                enter(FSM_PAIRING, 0);
            }
            else if (id == BUTTON_PAIR)
            {
                home_qr = !home_qr && cfg.self->link_count > 0;
                enter(FSM_HOME, 0);
            }
            break;
        case FSM_PROFILE:
            if (left)
            {
                enter(profile_back, 0);
            }
            else if (right)
            {
                link_focus = 0;
                enter(FSM_LINKS, 0);
            }
            break;
        case FSM_LINK_QR:
            if (left)
            {
                enter(FSM_LINKS, 0);
            }
            break;
        case FSM_WIFI_SETUP:
        case FSM_CLOCK:
            if (left)
            {
                enter(FSM_MENU, 0);
            }
            break;
        case FSM_PAIRING:
            if (pair_long)  // only waiting can be cancelled, as the old firmware
            {
                enter(FSM_HOME, 0);
            }
            break;
        case FSM_PEER_CARD:
            if (pair_long || left)
            {
                enter(FSM_HOME, 0);
            }
            break;
        default:
            break;  // connecting, transfer, messages, no SD: no keys
    }
}

// ── State changes ────────────────────────────────────────

static void leave(fsm_state_t next)
{
    stop_timers();
    if (state == FSM_WIFI_SETUP && next != FSM_WIFI_SETUP)
    {
        wifi_ap_stop();
    }
}

static void enter(fsm_state_t next, int arg)
{
    if (next != state)
    {
        ESP_LOGI(TAG, "%s -> %s", state_names[state], state_names[next]);
    }
    leave(next);
    state = next;

    switch (next)
    {
        case FSM_HOME:
            if (home_qr)
            {
                ui_show_qr(cfg.self->links[0].url);
            }
            else
            {
                ui_show_home(cfg.self, battery_level());
            }
            break;
        case FSM_MENU:
            ui_show_menu(&menu_actions, cfg.battery_mv, auto_sleep);
            return;
        case FSM_PROFILE:
            ui_show_contact_card(profile);
            break;
        case FSM_LINKS:
            ui_show_links(profile, link_focus, link_open, links_back);
            return;
        case FSM_LINK_QR:
            ui_show_qr(profile->links[link_focus].url);
            break;
        case FSM_FRIENDS:
        {
            int count = contacts_count_friends();
            for (int i = 0; i < count; i++)
            {
                friend_names[i] = contacts_friend_name(i);
            }
            if (friend_focus >= count)
            {
                friend_focus = count > 0 ? count - 1 : 0;
            }
            ui_show_friend_list(friend_names, count, friend_focus, contacts_load_friend,
                                &friend_actions);
            return;
        }
        case FSM_DELETE_CONFIRM:
            ui_show_delete_confirm(&friend_contact, delete_cancel, delete_confirm);
            return;
        case FSM_DELETED:
        {
            esp_err_t err = contacts_delete_friend(friend_focus);
            ui_show_message(err == ESP_OK ? "Friend deleted" : "Delete failed", NULL);
            start_timeout(err == ESP_OK ? DELETED_MS : DELETE_FAILED_MS);
            break;
        }
        case FSM_WIFI_SETUP:
        {
            // Starting takes a moment (the password needs the radio on).
            ui_show_message("Wi-Fi", "Starting...");
            ui_refresh_now();
            esp_err_t err = wifi_ap_start();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
                ui_show_message("Wi-Fi", "Failed to start");
                break;
            }
            wifi_step = 1;
            show_wifi_step();
            esp_timer_start_periodic(tick_timer, TICK_US);
            break;
        }
        case FSM_CLOCK:
            show_clock(false);
            esp_timer_start_periodic(tick_timer, TICK_US);
            break;
        case FSM_RESETTING:
        {
            // Old firmware: archive both profile directories, then reboot into
            // first-time setup.
            ui_show_message("Profile reset", "Rebooting...");
            ui_refresh_now();
            esp_err_t err = contacts_archive(NULL);
            if (err != ESP_OK)
            {
                ui_show_message("Reset failed", "Rebooting...");
            }
            start_timeout(err == ESP_OK ? RESET_MS : RESET_FAILED_MS);
            return;  // no keys: it reboots either way
        }
        case FSM_PAIRING:
            ui_show_pairing_wait(cfg.self);
            start_timeout(PAIRING_MS);
            break;
        case FSM_CONNECTING:
            ui_show_pairing_connecting(cfg.self);
            start_timeout(CONNECTING_MS);
            return;  // once BLE runs it runs to the end, as the old firmware
        case FSM_TRANSFER:
            ui_show_pairing_transfer(arg);
            return;
        case FSM_PEER_CARD:
            load_friend(arg);
            ui_show_contact_card(&friend_contact);
            start_timeout(PEER_CARD_MS);
            break;
        case FSM_NO_SD:
            ui_show_message("No SD Card", "Insert card to continue");
            return;
    }
    ui_set_key_handler(page_keys);
}

// ── Events from other tasks ──────────────────────────────

static void on_timeout(void)
{
    switch (state)
    {
        case FSM_DELETED:
            enter(FSM_FRIENDS, 0);
            break;
        case FSM_RESETTING:
            esp_restart();
            break;
        case FSM_PAIRING:
        case FSM_CONNECTING:
        case FSM_PEER_CARD:
            enter(FSM_HOME, 0);
            break;
        default:
            break;
    }
}

static void on_tick_event(void)
{
    if (state == FSM_CLOCK)
    {
        show_clock(true);
    }
    else if (state == FSM_WIFI_SETUP)
    {
        int step = wifi_ap_client_connected() ? 2 : 1;
        if (step != wifi_step)
        {
            wifi_step = step;
            show_wifi_step();
            ui_set_key_handler(page_keys);
        }
    }
}

static void handle(fsm_event_t event, int arg)
{
    switch (event)
    {
        case FSM_EV_TIMEOUT:
            if (arg == generation)
            {
                on_timeout();
            }
            break;
        case FSM_EV_TICK:
            on_tick_event();
            break;
        case FSM_EV_ACOM_DONE:
            if (state == FSM_PAIRING && arg)
            {
                enter(FSM_CONNECTING, 0);
            }
            break;
        case FSM_EV_BLE_PROGRESS:
            if (state == FSM_CONNECTING)
            {
                enter(FSM_TRANSFER, arg);
            }
            else if (state == FSM_TRANSFER)
            {
                ui_pairing_transfer_update(arg);
            }
            break;
        case FSM_EV_PAIRED:
            // A fast exchange may finish before any progress, so from
            // connecting too.
            if (state == FSM_CONNECTING || state == FSM_TRANSFER)
            {
                enter(arg >= 0 ? FSM_PEER_CARD : FSM_HOME, arg);
            }
            break;
        case FSM_EV_PAIR_FAILED:
            if (state == FSM_PAIRING || state == FSM_CONNECTING || state == FSM_TRANSFER)
            {
                enter(FSM_HOME, 0);
            }
            break;
    }
}

static bool drain_events(void)
{
    queued_t q;
    bool     any = false;
    while (xQueueReceive(queue, &q, 0) == pdTRUE)
    {
        handle(q.event, q.arg);
        any = true;
    }
    return any;
}

void fsm_post(fsm_event_t event, int arg)
{
    const queued_t q = {.event = event, .arg = arg};
    if (!queue || xQueueSend(queue, &q, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Event %d dropped", event);
        return;
    }
    ui_wake();
}

fsm_state_t fsm_state(void)
{
    return state;
}

bool fsm_auto_sleep(void)
{
    return auto_sleep;
}

void fsm_start(const fsm_config_t *config)
{
    cfg = *config;
    queue = xQueueCreate(QUEUE_LEN, sizeof(queued_t));
    const esp_timer_create_args_t tick_args = {.callback = on_tick, .name = "fsm_tick"};
    if (!queue || esp_timer_create(&tick_args, &tick_timer) != ESP_OK)
    {
        ESP_LOGE(TAG, "Out of memory");
        return;
    }
    ui_set_poll(drain_events);
    state = FSM_HOME;
    enter(cfg.sd_ok ? FSM_HOME : FSM_NO_SD, 0);
}
