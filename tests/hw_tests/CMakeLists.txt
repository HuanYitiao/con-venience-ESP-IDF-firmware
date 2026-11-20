# Every test file is built; each one is empty unless HW_TEST selects it.
idf_component_register(SRCS "hw_tests.c" "test_sleep.c" "test_display.c" "test_sd.c"
                            "test_audio.c" "test_acom.c" "test_ble.c" "test_wifi.c"
                            "test_contacts.c"
                       INCLUDE_DIRS "include"
                       REQUIRES button
                       PRIV_REQUIRES board power st75256 sdcard max98357a acom ble_link wifi_ap contacts
                                     esp_hw_support)
