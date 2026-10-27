idf_component_register(
    SRCS "cleaning_cycle.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer log freertos
        servo_driver water_sensor
)
