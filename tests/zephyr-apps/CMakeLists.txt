# The apps' C library on Zephyr: the apps' runtime (core/src/fc_apps.c) with
# the Zephyr port and its C library, as the firmware has them (port/zephyr's
# prj.conf and board files), without the agent. Built and run with
# tests/zephyr-apps.sh, on native_sim or mps2/an385 in QEMU.
cmake_minimum_required(VERSION 3.20)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(PORT ${ROOT}/port/zephyr)
# The firmware's configuration and devicetree, then this test's.
string(REPLACE "/" "_" FC_BOARD_FILE "${BOARD}")
set(CONF_FILE ${PORT}/prj.conf)
if(EXISTS ${PORT}/boards/${FC_BOARD_FILE}.conf)
    list(APPEND CONF_FILE ${PORT}/boards/${FC_BOARD_FILE}.conf)
endif()
list(APPEND CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj.conf)
if(EXISTS ${PORT}/boards/${FC_BOARD_FILE}.overlay)
    set(DTC_OVERLAY_FILE ${PORT}/boards/${FC_BOARD_FILE}.overlay)
endif()
list(APPEND DTS_ROOT ${PORT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fc_zephyr_apps)
zephyr_include_directories(${PORT}/include)

enable_language(ASM)
set(WAMR_BUILD_PLATFORM "zephyr")
if(CONFIG_ARCH_POSIX)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
        set(WAMR_BUILD_TARGET "AARCH64")
    else()
        set(WAMR_BUILD_TARGET "X86_64")
    endif()
elseif(CONFIG_CPU_CORTEX_M)
    set(WAMR_BUILD_TARGET "THUMBV7EM")
else()
    message(FATAL_ERROR "tests/zephyr-apps: native_sim or a Cortex-M board")
endif()
# As port/zephyr/CMakeLists.txt.
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_BUILD_TAIL_CALL 1)
set(WAMR_ROOT_DIR ${ROOT}/third_party/wamr)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

target_sources(app PRIVATE
    src/main.c
    ${PORT}/fc_port_zephyr.c
    ${PORT}/fc_hw_zephyr.c
    ${PORT}/fc_net_zephyr.c
    ${ROOT}/core/src/fc_log.c
    ${ROOT}/core/src/fc_appapi.c
    ${ROOT}/core/src/fc_arduino.c
    ${ROOT}/core/src/fc_board.c
    ${ROOT}/core/src/fc_metrics.c
    ${ROOT}/core/src/fc_http.c
    ${ROOT}/core/src/fc_mqtt.c
    ${ROOT}/third_party/cjson/cJSON.c
    ${WAMR_RUNTIME_LIB_SOURCE})
if(NOT CONFIG_COMMON_LIBC_MALLOC)
    target_sources(app PRIVATE ${PORT}/fc_heap_zephyr.c)
endif()
target_include_directories(app PRIVATE
    ${ROOT}/core/include ${ROOT}/core/src ${ROOT}/third_party/cjson ${PORT} src)
target_include_directories(app PRIVATE ${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
target_compile_definitions(app PRIVATE CJSON_NESTING_LIMIT=32 FC_LOG_KEEP=1)
set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES COMPILE_OPTIONS "-O2")
