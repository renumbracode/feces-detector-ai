cmake_minimum_required(VERSION 3.16)

# The Edge Impulse "ESP32-S3 C++ library" export (dropped into this directory)
# is registered as a sibling ESP-IDF component via the top-level
# EXTRA_COMPONENT_DIRS. When present, add it to REQUIRES and expose its root
# as an include dir so edge_impulse_invoke.c can reach the SDK headers.
set(_ei_reqs "")
set(_ei_inc_dirs ".")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/edge_impulse/CMakeLists.txt")
    set(_ei_reqs "edge_impulse")
    list(APPEND _ei_inc_dirs "edge_impulse")
endif()

idf_component_register(
    SRCS
        "inference_service.c"
        "edge_impulse_invoke.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${_ei_inc_dirs}
    REQUIRES espressif__esp32-camera espressif__esp_jpeg freertos esp_system esp_timer ${_ei_reqs}
)
