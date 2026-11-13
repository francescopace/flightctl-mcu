# flightctl-mcu on Zephyr: the agent core, the Zephyr port and a small
# application. Built with port/zephyr/build.sh.
cmake_minimum_required(VERSION 3.20)
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
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_ROOT_DIR ${ROOT}/third_party/wamr)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

# detools (third_party/fetch-detools.sh): firmware deltas, as in the other
# builds (CMakeLists.txt at the root).
set(DETOOLS ${ROOT}/third_party/detools/c)
set(DETOOLS_SRCS ${DETOOLS}/detools.c ${DETOOLS}/heatshrink/heatshrink_decoder.c)

target_sources(app PRIVATE
    src/main.c
    ${ROOT}/port/zephyr/fc_port_zephyr.c
    ${ROOT}/port/zephyr/fc_net_zephyr.c
    ${ROOT}/core/src/fc_agent.c
    ${ROOT}/core/src/fc_http.c
    ${ROOT}/core/src/fc_identity.c
    ${ROOT}/core/src/fc_oci.c
    ${ROOT}/core/src/fc_apps.c
    ${ROOT}/core/src/fc_resources.c
    ${ROOT}/core/src/fc_delta.c
    ${ROOT}/third_party/cjson/cJSON.c
    ${DETOOLS_SRCS}
    ${WAMR_RUNTIME_LIB_SOURCE})
if(CONFIG_FC_ENTROPY_RP2_ROSC)
    target_sources(app PRIVATE ${ROOT}/port/zephyr/fc_entropy_rp2.c)
endif()
# Infineon's WiFi driver (the Pico W's) keeps 20 packet buffers. Zephyr 4.5
# made them 2400 bytes each, 16 KB more than a board with 264 KB of RAM can
# give: 1600, what they were up to Zephyr 4.3 and what the board ran with, is
# an Ethernet frame and the chip's header.
if(CONFIG_WIFI_AIROC)
    zephyr_compile_definitions(AIROC_WIFI_PACKET_POOL_SIZE=1600)
endif()
# Raspberry Pi Pico boards: the USB console's conveniences.
if(CONFIG_SOC_FAMILY_RPI_PICO)
    target_sources(app PRIVATE src/board_rp2.c)
endif()
# The C heap is the port's unless the build keeps Zephyr's.
if(NOT CONFIG_COMMON_LIBC_MALLOC)
    target_sources(app PRIVATE ${ROOT}/port/zephyr/fc_heap_zephyr.c)
endif()
target_include_directories(app PRIVATE
    ${ROOT}/core/include ${ROOT}/core/src ${ROOT}/third_party/cjson ${ROOT}/port/zephyr
    ${DETOOLS} ${DETOOLS}/heatshrink)
# WAMR 2.4.5's Zephyr port includes <autoconf.h> and <version.h>, which Zephyr
# generates under zephyr/ and no longer puts on the include path as they are.
target_include_directories(app PRIVATE ${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
target_compile_definitions(app PRIVATE
    CJSON_NESTING_LIMIT=32
    DETOOLS_CONFIG_FILE_IO=0 DETOOLS_CONFIG_COMPRESSION_NONE=0 DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0 HEATSHRINK_DYNAMIC_ALLOC=1)
if(CONFIG_FC_AGENT_STACK_SIZE)
    target_compile_definitions(app PRIVATE FC_AGENT_STACK_SIZE=${CONFIG_FC_AGENT_STACK_SIZE})
endif()
set_source_files_properties(${DETOOLS_SRCS} PROPERTIES
    COMPILE_DEFINITIONS "malloc=fc_delta_alloc;free=fc_delta_free"
    COMPILE_OPTIONS "-w;-fwrapv")
