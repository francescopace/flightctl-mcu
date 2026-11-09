cmake_minimum_required(VERSION 3.16)
# Firmware version (esp_app_desc, reported as customInfo.firmwareVersion).
# Passed by build.sh as -D FW_VERSION=..., a cache variable, so a new value
# reconfigures the project.
set(FW_VERSION "dev" CACHE STRING "Firmware version")
set(PROJECT_VER "${FW_VERSION}")
# WAMR as an ESP-IDF component (third_party/fetch-wamr.sh).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../third_party/wamr/build-scripts/esp-idf")
# Build only what main needs, not every ESP-IDF component (a faster build).
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flightctl_mcu_fw)
# One cJSON in the firmware: ESP-IDF's (component "json", which WiFi
# provisioning needs too), with the agent's nesting limit. cJSON parses
# recursively, and its default of 1000 levels overflowed the agent's 24 KB
# thread stacks (RESULTS.md, problem 14: with two copies linked, the firmware
# ran ESP-IDF's, without the limit).
idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PUBLIC CJSON_NESTING_LIMIT=32)
