idf_component_register(
    SRCS
        "inference_service.c"
        "edge_impulse_invoke.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES espressif__esp32-camera espressif__esp_jpeg freertos esp_system esp_timer
)

## Edge Impulse exports a ready-to-build library folder (with its own
## CMakeLists.txt + model parameters). When present, import it so the
## tflite FOMO runtime is compiled in. See inference README.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/edge_impulse/CMakeLists.txt")
    # Not auto-imported by idf_component_register; extend if you vendor it.
endif()
