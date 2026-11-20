#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "button.h"
#include "contacts.h"
#include "esp_err.h"

// LVGL on the ST75256. All LVGL calls, including every ui_show_* function,
// stay in the task that runs ui_run().
//
// Keys: on pages with focusable widgets (menu, friend list) LVGL moves the
// focus: UP/DOWN move, RIGHT enters, LEFT goes back. Pages without them
// (home, card, clock, Wi-Fi setup) take the buttons through a key handler.

// Sets up LVGL, the display and the keypad. The display must already be
// up (st75256_init, backlight).
esp_err_t ui_init(void);

// Button events, from the button task. Handled in ui_run(); events before
// ui_init() are dropped.
void ui_on_button(button_id_t id, button_action_t action);

typedef void (*ui_key_handler_t)(button_id_t id, button_action_t action);

// Buttons for the page on screen, called in the LVGL task. NULL hands them
// to LVGL's focus instead. Every ui_show_* resets it to NULL.
void ui_set_key_handler(ui_key_handler_t handler);

// ── Pages ────────────────────────────────────────────────

// Home: the own avatar upside down between the battery side bands.
// battery_level: 0..BATTERY_LEVELS.
void ui_show_home(const contact_t *self, uint8_t battery_level);

// Contact card: avatar on the left, name, species and home on the right.
void ui_show_contact_card(const contact_t *c);

// Loads friend index for the list (contacts_load_friend fits).
typedef esp_err_t (*ui_friend_loader_t)(int index, contact_t *out);

typedef struct
{
    void (*open)(int index);    // RIGHT
    void (*remove)(int index);  // RIGHT long press
    void (*back)(void);         // LEFT
} ui_friend_actions_t;

// Friend list: names on the right, the focused friend's avatar on the left.
// Starts on friend focus. names must stay valid while the list is shown.
void ui_show_friend_list(const char *const *names, int count, int focus, ui_friend_loader_t load,
                         const ui_friend_actions_t *actions);

// A contact's links by platform, full width. Starts on link focus; RIGHT
// calls open, LEFT back. Without links: "No links".
void ui_show_links(const contact_t *c, int focus, void (*open)(int index), void (*back)(void));

// A QR code in the middle, between the full side bands: a link, or the own
// first link on the home page (PAIR click), as the old firmware.
void ui_show_qr(const char *text);

// Deleting a friend: the friend's avatar, "Delete friend?", the name, and
// Cancel / Delete, starting on Cancel.
void ui_show_delete_confirm(const contact_t *c, void (*cancel)(void), void (*confirm)(void));

// Blank screen (standby, as the old firmware).
void ui_show_standby(void);

// What the menu's entries do; the menu itself only moves between its pages
// (main, Settings, the reset confirmation).
typedef struct
{
    void (*profile)(void);
    void (*friends)(void);
    void (*wifi_setup)(void);
    void (*clock)(void);
    void (*auto_sleep)(bool on);
    void (*reset_profile)(void);
    void (*exit)(void);  // LEFT on the main page
} ui_menu_actions_t;

// The menu, back on the page and entry last shown; on its first entry after
// ui_menu_rewind(). battery_mv 0: no reading (shown as "--").
void ui_show_menu(const ui_menu_actions_t *actions, uint16_t battery_mv, bool auto_sleep);
void ui_menu_rewind(void);

// Clock page. Fail-closed as the old firmware: without trusted time it
// says so instead of showing a time.
typedef enum
{
    UI_CLOCK_OK,
    UI_CLOCK_NOT_SET,    // the RTC runs, but its time is not trusted
    UI_CLOCK_RTC_ERROR,  // the RTC does not answer
} ui_clock_status_t;

typedef struct
{
    ui_clock_status_t status;
    uint16_t          year;
    uint8_t           month, day, hours, minutes, seconds;
} ui_clock_t;

void ui_show_clock(const ui_clock_t *clock);
void ui_clock_update(const ui_clock_t *clock);  // the clock page must be on screen

// Wi-Fi setup, three steps as the old firmware: 1 join the AP (QR of the
// Wi-Fi string), 2 open the page (QR of the URL), 3 saved.
typedef struct
{
    int         step;     // 1..3
    const char *wifi_qr;  // "WIFI:T:WPA;S:...;P:...;;"
    const char *url;      // "http://192.168.4.1/"
    const char *self_id;  // shown on step 3
} ui_wifi_setup_t;

void ui_show_wifi_setup(const ui_wifi_setup_t *setup);

// Pairing, as the old firmware: waiting for contact, connecting (a ripple
// next to the own avatar, animating by itself), the transfer. percent < 0:
// no total known yet ("...").
void ui_show_pairing_wait(const contact_t *self);
void ui_show_pairing_connecting(const contact_t *self);
void ui_show_pairing_transfer(int percent);
void ui_pairing_transfer_update(int percent);  // the transfer page must be on screen

// A title and one line, centered (e.g. "Profile reset", "Rebooting...").
void ui_show_message(const char *title, const char *line);

// Called in the LVGL task on every pass of ui_run(), after LVGL has drawn.
// Returns true if it did something, so the loop runs again at once.
typedef bool (*ui_poll_t)(void);
void ui_set_poll(ui_poll_t poll);

// Draws the screen now, before slow work in the LVGL task (an AP start, an
// archive), so its page is up meanwhile.
void ui_refresh_now(void);

// Cuts ui_run()'s wait short, from any task (after posting it work).
void ui_wake(void);

// Runs LVGL. Does not return.
void ui_run(void);
