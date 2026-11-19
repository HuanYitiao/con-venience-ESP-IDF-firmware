#pragma once

#include "button.h"
#include "contacts.h"
#include "esp_err.h"

// LVGL on the ST75256. All LVGL calls, including the ui_show_* functions,
// stay in the task that runs ui_run().

// Sets up LVGL, the display and the keypad. The display must already be
// up (st75256_init, backlight).
esp_err_t ui_init(void);

// Button events, from the button task; queued for the LVGL keypad. Events
// before ui_init() are dropped.
void ui_on_button(button_id_t id, button_action_t action);

// Contact card: avatar on the left, name, species and home on the right.
void ui_show_contact_card(const contact_t *c);

// Runs LVGL. Does not return.
void ui_run(void);
