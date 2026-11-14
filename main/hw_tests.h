#pragma once

#include <stdbool.h>

#include "button.h"

// Hardware tests for the milestones, one at a time. Pick one with HW_TEST.
#define HW_TEST_NONE 0
// M2: shows the wake reason on the LED, then sleeps with a 30 s RTC wake.
// The board is off most of the time, so reflashing needs a wake first.
#define HW_TEST_SLEEP 1
// M5: test pattern, then the buttons tune the display drive settings.
#define HW_TEST_DISPLAY_TUNE 2
// M6: card detect, mount, old card read, write/read, display and SD in turns.
#define HW_TEST_SD 3

#define HW_TEST HW_TEST_SD

// Runs the selected test after the boot sequence. display_ok: the display
// (and with it +3.3V_PERIPH and the SPI bus) is up.
void hw_test_run(bool display_ok);

// Button events, for tests that use the buttons.
void hw_test_on_button(button_id_t id, button_action_t action);
