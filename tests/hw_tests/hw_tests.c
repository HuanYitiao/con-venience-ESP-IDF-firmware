#include "hw_tests_priv.h"

void hw_test_run(bool display_ok)
{
#if HW_TEST == HW_TEST_SLEEP
    test_sleep_run();
#elif HW_TEST == HW_TEST_DISPLAY_TUNE
    if (display_ok)
    {
        test_display_run();
    }
#elif HW_TEST == HW_TEST_SD
    test_sd_run(display_ok);
#elif HW_TEST == HW_TEST_AUDIO
    test_audio_run();  // needs +3.3V_PERIPH, which the display start switched on
#elif HW_TEST == HW_TEST_ACOM
    test_acom_run();
#elif HW_TEST == HW_TEST_BLE
    test_ble_run();
#elif HW_TEST == HW_TEST_WIFI
    test_wifi_run();
#elif HW_TEST == HW_TEST_CONTACTS
    test_contacts_run();
#endif
    (void)display_ok;
}

void hw_test_on_button(button_id_t id, button_action_t action)
{
#if HW_TEST == HW_TEST_DISPLAY_TUNE
    test_display_on_button(id, action);
#elif HW_TEST == HW_TEST_WIFI
    test_wifi_on_button(id, action);
#endif
    (void)id;
    (void)action;
}
