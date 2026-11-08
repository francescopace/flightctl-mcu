# Host build of the agent core ("fake device" for development and CI).
# The firmware is built separately with ESP-IDF (see firmware/).
cmake_minimum_required(VERSION 3.16)
project(flightctl_mcu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# mbedTLS 3.6 matches the version bundled with ESP-IDF 5.5.
# macOS: brew install mbedtls@3, then -DMbedTLS_DIR=$(brew --prefix mbedtls@3)/lib/cmake/MbedTLS
find_package(MbedTLS 3.6 REQUIRED)
find_package(Threads REQUIRED)

# WAMR (third_party/fetch-wamr.sh): fast interpreter only, no WASI/libc,
# like the firmware build.
set(WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wamr)
if(APPLE)
    set(WAMR_BUILD_PLATFORM "darwin")
else()
    set(WAMR_BUILD_PLATFORM "linux")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
    set(WAMR_BUILD_TARGET "AARCH64")
else()
    set(WAMR_BUILD_TARGET "X86_64")
endif()
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(vmlib PRIVATE -w)
if(NOT APPLE)
    target_link_libraries(vmlib PUBLIC m dl Threads::Threads)
endif()

add_library(fc_core STATIC
    core/src/fc_agent.c
    core/src/fc_http.c
    core/src/fc_identity.c
    core/src/fc_oci.c
    core/src/fc_apps.c
    core/third_party/cjson/cJSON.c
)
target_include_directories(fc_core PUBLIC core/include core/third_party/cjson PRIVATE core/src)
target_include_directories(fc_core PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include)
target_link_libraries(fc_core PUBLIC vmlib MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto Threads::Threads)

add_executable(fc-host-agent host/main.c port/host/fc_port_host.c)
target_link_libraries(fc-host-agent PRIVATE fc_core)

# Fuzzers (clang with libFuzzer): cmake -DFC_FUZZ=ON -DCMAKE_C_COMPILER=clang ...
# then ci/fuzz.sh runs them. Each one compiles the sources it tests, with
# the sanitizers on.
option(FC_FUZZ "Build the libFuzzer targets" OFF)
if(FC_FUZZ)
    set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1)
    add_compile_options(-Wno-deprecated-declarations)
    set(CJSON core/third_party/cjson/cJSON.c)
    function(fc_fuzzer name)
        add_executable(${name} tests/fuzz/${name}.c ${ARGN})
        target_include_directories(${name} PRIVATE core/include core/src core/third_party/cjson
                                   ${WAMR_ROOT_DIR}/core/iwasm/include)
        target_compile_options(${name} PRIVATE ${FUZZ_FLAGS})
        target_link_options(${name} PRIVATE ${FUZZ_FLAGS})
        target_link_libraries(${name} PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto Threads::Threads)
    endfunction()
    fc_fuzzer(fuzz_http ${CJSON})
    fc_fuzzer(fuzz_oci port/host/fc_port_host.c ${CJSON})
    fc_fuzzer(fuzz_agent core/src/fc_http.c core/src/fc_identity.c core/src/fc_apps.c port/host/fc_port_host.c ${CJSON})
    target_link_libraries(fuzz_agent PRIVATE vmlib)
endif()
