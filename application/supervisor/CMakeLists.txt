idf_component_register(SRCS "supervisor.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES fsm power sdcard wifi_ap esp_timer esp_system)
