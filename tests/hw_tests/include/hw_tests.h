#pragma once

#include <stdbool.h>

#include "button.h"

// Hardware tests for the middleware milestones, one at a time, run by
// app_main after the boot sequence. Pick one with HW_TEST below; keep it at
// HW_TEST_NONE in commits. Each test lives in its own test_*.c, compiled
// only when selected.
#define HW_TEST_NONE 0
// M2: shows the wake reason on the LED, then sleeps with a 30 s RTC wake.
// The board is off most of the time, so reflashing needs a wake first.
#define HW_TEST_SLEEP 1
// M5: test pattern, then the buttons tune the display drive settings.
#define HW_TEST_DISPLAY_TUNE 2
// M6: card detect, mount, old card read, write/read, display and SD in turns.
#define HW_TEST_SD 3
// M7: the old startup beep, then a short melody as square and as sine wave.
// Listen for clicks at the start and end of each.
#define HW_TEST_AUDIO 4
// M8: ACOM exchange with our BT MAC, repeated. Touch an old-firmware
// device in pairing mode to the contact.
#define HW_TEST_ACOM 5
// M9: ACOM, then a BLE transfer of a test profile ("M9 Test", gradient
// avatar) in the old packing format. Role by MAC, as the old firmware.
#define HW_TEST_BLE 6
// M10: soft AP with captive portal and a test page with a file upload.
// BLE is started first, as in the real flow. PAIR long press: AP off/on.
#define HW_TEST_WIFI 7
// A2: contacts on the card: all friends read back, own uuid and packing,
// then save, replace, interrupted save, bad packages, delete and archive on
// a test friend. Leaves the card as it was.
#define HW_TEST_CONTACTS 8

#define HW_TEST HW_TEST_NONE

// Runs the selected test after the boot sequence. display_ok: the display
// (and with it +3.3V_PERIPH and the SPI bus) is up.
void hw_test_run(bool display_ok);

// Button events, for tests that use the buttons.
void hw_test_on_button(button_id_t id, button_action_t action);
