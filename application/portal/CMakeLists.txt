idf_component_register(SRCS "portal.c" "portal_pages.c"
                       INCLUDE_DIRS "include"
                       REQUIRES contacts
                       PRIV_REQUIRES wifi_ap pcf85063a sdcard esp_http_server)
