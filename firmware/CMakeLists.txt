cmake_minimum_required(VERSION 3.16)
# Firmware version (esp_app_desc, reported as customInfo.firmwareVersion).
# Passed by build.sh as -D FW_VERSION=..., a cache variable, so a new value
# reconfigures the project.
set(FW_VERSION "dev" CACHE STRING "Firmware version")
set(PROJECT_VER "${FW_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flightctl_mcu_fw)
