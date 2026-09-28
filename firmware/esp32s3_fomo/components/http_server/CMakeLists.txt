idf_component_register(
    SRCS "http_server_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer esp_wifi esp_system freertos
        camera inference spray app_config detection_service
)
