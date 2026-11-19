#pragma once

#include <stdint.h>

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

// Home: the own avatar upside down between the battery side bands.
// battery_level: 0..BATTERY_LEVELS.
void ui_show_home(const contact_t *self, uint8_t battery_level);

// Contact card: avatar on the left, name, species and home on the right.
void ui_show_contact_card(const contact_t *c);

// Loads friend index for the list (contacts_load_friend fits).
typedef esp_err_t (*ui_friend_loader_t)(int index, contact_t *out);

// Friend list: names on the right, the focused friend's avatar on the left.
// names must stay valid while the list is shown.
void ui_show_friend_list(const char *const *names, int count, ui_friend_loader_t load);

// Until the page flow (fsm) exists: PAIR long press cycles home -> card ->
// friend list. Everything pointed to must stay valid.
typedef struct
{
    const contact_t    *self;
    uint8_t             battery_level;
    const char *const  *friend_names;
    int                 friend_count;
    ui_friend_loader_t  load_friend;
} ui_demo_t;

void ui_demo_start(const ui_demo_t *demo);

// Runs LVGL. Does not return.
void ui_run(void);
