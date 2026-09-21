idf_component_register(
    SRCS "camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp32-camera freertos esp_system
)
