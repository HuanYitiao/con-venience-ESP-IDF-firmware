#pragma once

#include <stdbool.h>

#include "button.h"
#include "hw_tests.h"

// One entry point per test, in test_*.c; only the selected one is built.
void test_sleep_run(void);
void test_display_run(void);
void test_display_on_button(button_id_t id, button_action_t action);
void test_sd_run(bool display_ok);
void test_audio_run(void);
void test_acom_run(void);
void test_ble_run(void);
void test_wifi_run(void);
void test_wifi_on_button(button_id_t id, button_action_t action);
