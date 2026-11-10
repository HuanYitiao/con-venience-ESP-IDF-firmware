#pragma once

#include "esp_err.h"

typedef enum
{
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_PAIR,
    BUTTON_COUNT,
} button_id_t;

typedef enum
{
    BUTTON_CLICK,       // released before BUTTON_LONG_PRESS_MS
    BUTTON_LONG_PRESS,  // released after it
} button_action_t;

// Runs in the button task: keep it short, post to a queue rather than doing work here.
typedef void (*button_cb_t)(button_id_t id, button_action_t action);

// Call after power_early_init(): reading GPIOB clears the INTF bits the wake
// reason is taken from. A button already held here is ignored until released.
esp_err_t button_init(button_cb_t cb);
