idf_component_register(
    SRCS "servo_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc esp_driver_gpio log
)
