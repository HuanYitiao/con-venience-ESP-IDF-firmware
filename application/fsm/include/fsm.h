#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "contacts.h"

// The page flow: which page is on screen and what leads where, as the old
// firmware. Focus inside a page (menu entries, list rows, links) is LVGL's.
// Runs in the LVGL task: pages report their choices to it directly, other
// tasks post events (fsm_post), which it handles on the next pass of
// ui_run().

typedef enum
{
    FSM_HOME,            // own avatar, or the own first link as a QR code
    FSM_MENU,            // main, Settings and the reset confirmation (ui_menu)
    FSM_PROFILE,         // a contact card: own or a friend's
    FSM_LINKS,           // that contact's links
    FSM_LINK_QR,         // one link as a QR code
    FSM_FRIENDS,         // friend list
    FSM_DELETE_CONFIRM,  // "Delete friend?"
    FSM_DELETED,         // "Friend deleted", briefly
    FSM_WIFI_SETUP,      // AP up, steps for the phone
    FSM_CLOCK,           // the RTC's time
    FSM_RESETTING,       // profile archived, reboot follows
    FSM_PAIRING,         // waiting for contact (ACOM)
    FSM_CONNECTING,      // BLE link coming up
    FSM_TRANSFER,        // BLE transfer, with progress
    FSM_PEER_CARD,       // the friend just paired, for a while
    FSM_NO_SD,           // no card: nothing works
} fsm_state_t;

// Events from other tasks.
typedef enum
{
    FSM_EV_TIMEOUT,      // internal: the state's timer (arg: its generation)
    FSM_EV_TICK,         // internal: once a second on the clock and Wi-Fi pages
    FSM_EV_PAIR_CONTACT,   // internal: ACOM got the peer, BLE starts
    FSM_EV_PAIR_PROGRESS,  // internal: arg percent, -1 without a total
    FSM_EV_PAIR_DONE,      // internal: arg 1 the peer's profile arrived, 0 failed
} fsm_event_t;

typedef struct
{
    contact_t       *self;          // must stay valid; reloaded after a portal save
    uint16_t         battery_mv;    // boot reading for the menu, 0: none
    bool             sd_ok;         // card mounted: else only the "No SD Card" page
    bool             provisioning;  // no usable own profile: first-time setup
} fsm_config_t;

// Shows the first page and hooks into ui_run(). After ui_init().
void fsm_start(const fsm_config_t *config);

// From any task.
void fsm_post(fsm_event_t event, int arg);

fsm_state_t fsm_state(void);

// Settings > Auto sleep; RAM only, on after every boot (old firmware).
bool fsm_auto_sleep(void);
