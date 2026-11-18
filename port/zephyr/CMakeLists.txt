# flightctl-mcu on Zephyr: the agent core, the Zephyr port and a small
# application. Built with port/zephyr/build.sh.
cmake_minimum_required(VERSION 3.20)
# The flightctl,app-hw binding (dts/bindings): the board map's names.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flightctl_mcu)
# For mbedTLS too: it includes fc-mbedtls-user.h (prj.conf).
zephyr_include_directories(include)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# WAMR (third_party/fetch-wamr.sh): fast interpreter only, as in the other
# builds.
enable_language(ASM)
set(WAMR_BUILD_PLATFORM "zephyr")
if(CONFIG_ARCH_POSIX)
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
        set(WAMR_BUILD_TARGET "AARCH64")
    else()
        set(WAMR_BUILD_TARGET "X86_64")
    endif()
elseif(CONFIG_CPU_CORTEX_M0PLUS OR CONFIG_CPU_CORTEX_M0)
    set(WAMR_BUILD_TARGET "THUMBV6M")
elseif(CONFIG_CPU_CORTEX_M)
    set(WAMR_BUILD_TARGET "THUMBV7EM")
elseif(CONFIG_XTENSA)
    set(WAMR_BUILD_TARGET "XTENSA")
else()
    message(FATAL_ERROR "flightctl-mcu: no WAMR target for this board yet")
endif()
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
# libc-builtin: a few C library functions for the apps (fc_apps.c, s_libc).
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
# Instruction metering: an app call that runs past its budget traps, so an
# app in an endless loop can always be stopped (core/src/fc_apps.c).
set(WAMR_BUILD_INSTRUCTION_METERING 1)
# Tail calls (return_call), which clang emits with -mtail-call and current
# toolchains by default: CoreMark as wasm-coremark publishes it uses them.
set(WAMR_BUILD_TAIL_CALL 1)
set(WAMR_ROOT_DIR ${ROOT}/third_party/wamr)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

target_sources(app PRIVATE
    src/main.c
    ${ROOT}/port/zephyr/fc_port_zephyr.c
    ${ROOT}/port/zephyr/fc_net_zephyr.c
    ${ROOT}/port/zephyr/fc_hw_zephyr.c
    ${ROOT}/core/src/fc_agent.c
    ${ROOT}/core/src/fc_rendered.c
    ${ROOT}/core/src/fc_console.c
    ${ROOT}/core/src/fc_log.c
    ${ROOT}/core/src/fc_metrics.c
    ${ROOT}/core/src/fc_mqtt.c
    ${ROOT}/core/src/fc_http.c
    ${ROOT}/core/src/fc_identity.c
    ${ROOT}/core/src/fc_oci.c
    ${ROOT}/core/src/fc_apps.c
    ${ROOT}/core/src/fc_appapi.c
    ${ROOT}/core/src/fc_arduino.c
    ${ROOT}/core/src/fc_board.c
    ${ROOT}/core/src/fc_resources.c
    ${ROOT}/core/src/fc_delta.c
    ${ROOT}/third_party/cjson/cJSON.c
    ${WAMR_RUNTIME_LIB_SOURCE})
# Boards with WiFi: the network, given at the factory, or in the field where
# the board can open an access point (with a DHCP server for installers).
if(CONFIG_WIFI)
    target_sources(app PRIVATE src/wifi.c)
endif()
if(CONFIG_WIFI AND CONFIG_NET_DHCPV4_SERVER)
    target_sources(app PRIVATE ${ROOT}/core/src/fc_prov.c)
endif()
# The C heap is the port's unless the build keeps Zephyr's.
if(NOT CONFIG_COMMON_LIBC_MALLOC)
    target_sources(app PRIVATE ${ROOT}/port/zephyr/fc_heap_zephyr.c)
    # The S3 with PSRAM and software crypto can receive TLS records in
    # external RAM. A hardware accelerator may require DMA-capable memory.
    if(CONFIG_BOARD_ESP32S3_DEVKITC_ESP32S3_PROCPU AND CONFIG_ESP_SPIRAM AND CONFIG_SPIRAM_MODE_OCT
       AND CONFIG_ESP_SPIRAM_SIZE EQUAL 8388608 AND CONFIG_SHARED_MULTI_HEAP AND NOT CONFIG_CRYPTO)
        set_source_files_properties(${ROOT}/core/src/fc_http.c ${ROOT}/port/zephyr/fc_heap_zephyr.c
            PROPERTIES COMPILE_DEFINITIONS FC_TLS_POOL_PORT_STORAGE)
    endif()
endif()
target_include_directories(app PRIVATE
    ${ROOT}/core/include ${ROOT}/core/src ${ROOT}/third_party/cjson ${ROOT}/port/zephyr)
# WAMR 2.4.5's Zephyr port includes <autoconf.h> and <version.h>, which Zephyr
# generates under zephyr/ and no longer puts on the include path as they are.
target_include_directories(app PRIVATE ${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
# On Xtensa the fatal error handler walks the stack with Zephyr's own
# backtrace, whose headers are the architecture's (fc_port_zephyr.c).
if(CONFIG_XTENSA)
    target_include_directories(app PRIVATE ${ZEPHYR_BASE}/arch/xtensa/include)
endif()
# FC_LOG_KEEP: the device's log in RAM that a restart keeps (fc_log.h).
target_compile_definitions(app PRIVATE
    CJSON_NESTING_LIMIT=32
    FC_LOG_KEEP=1)
if(CONFIG_FC_CONSOLE_CRASH)
    target_compile_definitions(app PRIVATE FC_CONSOLE_CRASH=1)
endif()
if(CONFIG_FC_AGENT_STACK_SIZE)
    target_compile_definitions(app PRIVATE FC_AGENT_STACK_SIZE=${CONFIG_FC_AGENT_STACK_SIZE})
endif()
# WAMR, which runs the apps, optimized for speed (-O2, after Zephyr's -Os);
# the rest stays optimized for size. With -O2 for everything the ESP32-S3's
# data is 5.6 KB larger than its RAM.
set_source_files_properties(${WAMR_RUNTIME_LIB_SOURCE} PROPERTIES COMPILE_OPTIONS "-O2")
