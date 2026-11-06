cmake_minimum_required(VERSION 3.16)
# Firmware version (esp_app_desc, reported as customInfo.firmwareVersion).
# Passed by build.sh as -D FW_VERSION=..., a cache variable, so a new value
# reconfigures the project.
set(FW_VERSION "dev" CACHE STRING "Firmware version")
set(PROJECT_VER "${FW_VERSION}")
# WAMR as an ESP-IDF component (third_party/fetch-wamr.sh).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../third_party/wamr/build-scripts/esp-idf")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flightctl_mcu_fw)
