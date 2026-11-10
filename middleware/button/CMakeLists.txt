idf_component_register(SRCS "button.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board mcp23017 esp_driver_gpio freertos)