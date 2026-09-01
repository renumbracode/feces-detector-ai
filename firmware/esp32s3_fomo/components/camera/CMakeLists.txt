idf_component_register(
    SRCS "camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_camera freertos esp_system
)
