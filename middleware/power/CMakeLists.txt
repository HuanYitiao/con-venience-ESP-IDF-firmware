idf_component_register(SRCS "power.c" "battery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board mcp23017 pcf85063a esp_driver_gpio esp_adc nvs_flash)
