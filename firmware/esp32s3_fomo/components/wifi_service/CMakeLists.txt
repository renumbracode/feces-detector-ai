idf_component_register(
    SRCS "wifi_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event freertos nvs_flash app_config
)
