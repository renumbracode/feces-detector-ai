idf_component_register(
    SRCS "target_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer log freertos esp_system
        main_config geometry servo_driver water_sensor cleaning_cycle
)
