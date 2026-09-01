idf_component_register(
    SRCS "detection_service.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_http_client esp32_camera esp_system
        camera inference spray app_config
)
