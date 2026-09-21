idf_component_register(
    SRCS "detection_service.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_http_client espressif__esp32-camera esp_system
        camera inference spray app_config
)
