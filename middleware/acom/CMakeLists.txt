idf_component_register(SRCS "acom.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board esp_driver_uart esp_driver_gpio esp_rom esp_hw_support)
