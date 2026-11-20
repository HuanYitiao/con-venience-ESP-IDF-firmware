#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Pairing, as the old firmware: touch two devices (ACOM swaps their BLE
// addresses), then BLE swaps the profiles (the lower address is the
// server), and the peer's profile is saved as a friend.

typedef enum
{
    PAIRING_EV_CONTACT,   // ACOM got the peer's address; BLE starts
    PAIRING_EV_PROGRESS,  // arg: percent, -1 while transferring without a total
    PAIRING_EV_DONE,      // arg: 1 the peer's profile arrived, 0 failed
} pairing_event_t;

// Called from the ACOM, BLE and timer tasks: post it on, keep it short.
typedef void (*pairing_cb_t)(pairing_event_t event, int arg);

// The BLE host and ACOM. At boot; needs NVS.
esp_err_t pairing_init(void);

// Packs the own profile and waits for contact; ACOM tries again until
// pairing_stop(). From the task that owns the SD card.
// ESP_ERR_INVALID_STATE: no own profile to send (no uuid or avatar).
esp_err_t pairing_start(pairing_cb_t cb);

// Ends whatever runs, without further events. Not from inside cb.
void pairing_stop(void);

// After PAIRING_EV_DONE with 1: saves the peer as a friend. From the task
// that owns the SD card. Returns the friend's index in the new list, -1 if
// the profile is malformed or does not save.
int pairing_save(void);
