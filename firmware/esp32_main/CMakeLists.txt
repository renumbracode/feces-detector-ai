cmake_minimum_required(VERSION 3.16)

# The main ESP32 (Laptop B): owns the pump relay, the two nozzle servos and
# the water-level sensor. It receives image coordinates from the camera board
# over the shared Wi-Fi and does the angle conversion itself.
#
# Target: ESP32-S3, matching the camera board so one toolchain and one set of
# docs covers both. To retarget a plain ESP32, change the two lines below and
# regenerate sdkconfig.
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_main)
