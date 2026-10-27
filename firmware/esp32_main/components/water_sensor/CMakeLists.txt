idf_component_register(
    SRCS "water_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc esp_driver_gpio log
)
