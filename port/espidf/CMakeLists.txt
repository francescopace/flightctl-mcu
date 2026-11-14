cmake_minimum_required(VERSION 3.16)
# Firmware version (esp_app_desc, reported as customInfo.firmwareVersion).
# Passed by build.sh as -D FW_VERSION=..., a cache variable, so a new value
# reconfigures the project.
set(FW_VERSION "dev" CACHE STRING "Firmware version")
set(PROJECT_VER "${FW_VERSION}")
# WAMR as an ESP-IDF component (third_party/fetch-wamr.sh).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../third_party/wamr/build-scripts/esp-idf")
# Build only what main needs, not every ESP-IDF component (a faster build).
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The components that are not ESP-IDF's (main/idf_component.yml) are pinned,
# with their hashes, in a lock file per chip: the component manager solves
# again when the file is for another chip, and one file would change at every
# build. (The chip as build.sh gives it: ESP-IDF settles it only in project().)
set(fc_target "${IDF_TARGET}")
if(NOT fc_target)
    set(fc_target "$ENV{IDF_TARGET}")
endif()
if(fc_target)
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${fc_target}")
endif()
project(flightctl_mcu_fw)
# What WAMR 2.4.5's ESP-IDF port needs to build on ESP-IDF 6 (GCC 15,
# picolibc), without touching its sources:
# - wamr_idf6.h, forced into every file: the declarations it took from
#   newlib's headers without including them;
# - -Wno-format-security, which must go with the -Wno-format it sets (fixed in
#   WAMR after 2.4.5);
# - MALLOC_CAP_EXEC, which ESP-IDF 6 does not define while memory protection
#   is on: executable memory is for AOT code, which this firmware does not
#   run, and asking for it now fails as it did at run time.
idf_component_get_property(wamr_lib wamr COMPONENT_LIB)
target_compile_options(${wamr_lib} PRIVATE -include ${CMAKE_CURRENT_LIST_DIR}/wamr_idf6.h -Wno-format-security)
if(NOT CONFIG_HEAP_HAS_EXEC_HEAP)
    target_compile_definitions(${wamr_lib} PRIVATE MALLOC_CAP_EXEC=MALLOC_CAP_INVALID)
endif()
# Instruction metering, which WAMR's ESP-IDF component has no option for: an
# app call that runs past its budget traps, so an app in an endless loop can
# always be stopped (core/src/fc_apps.c). Public, so the core sees it too.
target_compile_definitions(${wamr_lib} PUBLIC WASM_ENABLE_INSTRUCTION_METERING=1)
