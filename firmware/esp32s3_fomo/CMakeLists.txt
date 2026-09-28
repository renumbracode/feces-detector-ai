cmake_minimum_required(VERSION 3.16)

# Edge Impulse "ESP32 / ESP32-S3 C++ library" exports ship a ready-to-build
# component. Drop the unzipped export under
#   components/inference/edge_impulse/
# and it is added here and built automatically; without it the firmware
# falls back to the brightness stub (model reports as "fomo:stub").
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/inference/edge_impulse/CMakeLists.txt")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/inference/edge_impulse/")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_fomo)
