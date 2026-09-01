idf_component_register(
    SRCS "spray_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos esp_system
)
