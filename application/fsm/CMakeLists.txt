idf_component_register(SRCS "fsm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES contacts
                       PRIV_REQUIRES ui power pcf85063a wifi_ap esp_timer)
