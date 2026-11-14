idf_component_register(SRCS "sdcard.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board mcp23017 fatfs esp_driver_sdspi sdmmc)
