cmake_minimum_required(VERSION 3.20)
project(flightctl_nuttx C ASM)
set(ROOT "$ENV{FC_ROOT}")
set(WAMR_ROOT_DIR "${ROOT}/third_party/wamr" CACHE PATH "Patched WAMR source tree")
set(FC_NUTTX_CHIP esp32s3 CACHE STRING "NuttX target SoC")
set(WAMR_BUILD_PLATFORM nuttx)
if(FC_NUTTX_CHIP MATCHES "^esp32c[356]$")
    set(WAMR_BUILD_TARGET RISCV32_ILP32)
elseif(FC_NUTTX_CHIP MATCHES "^(rp23xx|nrf53|stm32f7)$")
    set(WAMR_BUILD_TARGET THUMB)
else()
    set(WAMR_BUILD_TARGET XTENSA)
endif()
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_BUILD_TAIL_CALL 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(fc_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(fc_wamr PRIVATE -O2)
target_compile_definitions(fc_wamr PRIVATE WASM_ENABLE_COMPACT_DISPATCH=1)

set(ENABLE_PROGRAMS OFF CACHE BOOL "")
set(ENABLE_TESTING OFF CACHE BOOL "")
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "")
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tls_config.h" CACHE FILEPATH "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/psa_config.h" CACHE FILEPATH "" FORCE)
# NuttX supplies the POSIX socket and time interfaces used by mbedTLS.
add_compile_definitions(__unix__)
add_subdirectory("$ENV{FC_MBEDTLS}" mbedtls EXCLUDE_FROM_ALL)
set(core "${ROOT}/core/src")
set_property(SOURCE ${core}/fc_apps.c APPEND PROPERTY COMPILE_DEFINITIONS FC_APP_STATE_LOG_BUFFER_ONLY)
add_library(fc_nuttx STATIC
    fc_ota_arm.c fc_ota_arm_flash.c
    fc_port_nuttx.c fc_ota_nuttx.c ${core}/fc_agent.c ${core}/fc_rendered.c ${core}/fc_console.c
    ${core}/fc_log.c ${core}/fc_metrics.c ${core}/fc_mqtt.c ${core}/fc_http.c
    ${core}/fc_identity.c ${core}/fc_oci.c ${core}/fc_apps.c ${core}/fc_appapi.c
    ${core}/fc_arduino.c ${core}/fc_board.c ${core}/fc_resources.c ${core}/fc_delta.c
    ${ROOT}/third_party/cjson/cJSON.c)
target_include_directories(fc_nuttx PRIVATE ${ROOT}/core/include ${core}
    ${ROOT}/third_party/cjson ${WAMR_ROOT_DIR}/core/iwasm/include)
if(FC_NUTTX_CHIP STREQUAL "esp32s3")
    target_sources(fc_nuttx PRIVATE fc_hw_nuttx.c fc_sensor_nuttx.c fc_bus_nuttx.c fc_crash_nuttx.c)
    set(hal "$ENV{FC_NUTTX_HAL}")
    target_include_directories(fc_nuttx SYSTEM PRIVATE
        ${hal}/nuttx/esp32s3/include ${hal}/components/soc/esp32s3/include
        ${hal}/components/esp_hal_gpio/esp32s3/include ${hal}/components/esp_hal_gpio/include
        ${hal}/components/upper_hal_rmt/include ${hal}/components/esp_hal_rmt/include
        ${hal}/components/soc/include ${hal}/components/esp_common/include)
else()
    target_sources(fc_nuttx PRIVATE fc_hw_basic_nuttx.c)
    if(FC_NUTTX_CHIP MATCHES "^(esp32c[356]|rp23xx|nrf53)$")
        target_sources(fc_nuttx PRIVATE fc_sensor_nuttx.c)
    endif()
endif()
if(FC_NUTTX_CHIP STREQUAL "stm32f7")
    target_sources(fc_nuttx PRIVATE fc_storage_nucleo.c fc_sensor_nuttx.c)
endif()
target_compile_definitions(fc_nuttx PRIVATE CJSON_NESTING_LIMIT=32 FC_HEAP_HOLD_MAX=131072)
target_link_libraries(fc_nuttx PRIVATE mbedtls mbedx509 tfpsacrypto fc_wamr)
