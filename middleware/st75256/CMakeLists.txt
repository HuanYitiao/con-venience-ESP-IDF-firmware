idf_component_register(SRCS "st75256.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board mcp23017 esp_lcd esp_driver_ledc)
