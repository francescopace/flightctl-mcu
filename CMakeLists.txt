# Host build of the agent core ("fake device" for development and CI).
# The firmware is built separately with ESP-IDF (see firmware/).
cmake_minimum_required(VERSION 3.16)
project(flightctl_mcu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# mbedTLS 4.1, the long-term support branch that ESP-IDF 6.0 and Zephyr 4.5
# have, or later. Its cryptography is PSA only (TF-PSA-Crypto).
# macOS: brew install mbedtls
find_package(MbedTLS 4.1 REQUIRED)
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
# libc-builtin: a few C library functions for the apps (fc_apps.c, s_libc).
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
# Instruction metering: an app call that runs past its budget traps, so an
# app in an endless loop can always be stopped (fc_apps.c).
set(WAMR_BUILD_INSTRUCTION_METERING 1)
# Tail calls (return_call), which clang emits with -mtail-call and current
# toolchains by default: CoreMark as wasm-coremark publishes it uses them.
set(WAMR_BUILD_TAIL_CALL 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(vmlib PRIVATE -w)
if(NOT APPLE)
    target_link_libraries(vmlib PUBLIC m dl Threads::Threads)
endif()

# Firmware deltas: detools' sequential patches, compressed with heatshrink,
# applied by the core's own decoder.
set(DELTA_SRCS core/src/fc_delta.c)

add_library(fc_core STATIC
    core/src/fc_agent.c
    core/src/fc_console.c
    core/src/fc_log.c
    core/src/fc_metrics.c
    core/src/fc_mqtt.c
    core/src/fc_http.c
    core/src/fc_identity.c
    core/src/fc_oci.c
    core/src/fc_apps.c
    core/src/fc_appapi.c
    core/src/fc_board.c
    core/src/fc_resources.c
    core/src/fc_prov.c
    third_party/cjson/cJSON.c
    ${DELTA_SRCS}
)
target_include_directories(fc_core PUBLIC core/include third_party/cjson PRIVATE core/src)
# Same cJSON nesting limit as the firmware (port/espidf/components/fc_core).
target_compile_definitions(fc_core PUBLIC CJSON_NESTING_LIMIT=32)
target_include_directories(fc_core PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include)
target_link_libraries(fc_core PUBLIC vmlib MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto Threads::Threads)

add_executable(fc-host-agent port/host/main.c port/host/fc_port_host.c)
target_link_libraries(fc-host-agent PRIVATE fc_core)

# The field provisioning server with a pretend radio (tests/e2e/prov-host.sh).
add_executable(fc-prov-host port/host/prov_main.c port/host/fc_port_host.c)
target_link_libraries(fc-prov-host PRIVATE fc_core)

# The device's registry client alone, against a real registry
# (tests/registry-bearer.sh). Built from the sources it needs only.
add_executable(fc-oci-pull EXCLUDE_FROM_ALL tests/registry/oci_pull.c port/host/fc_port_host.c core/src/fc_http.c
               core/src/fc_log.c third_party/cjson/cJSON.c ${DELTA_SRCS})
target_include_directories(fc-oci-pull PRIVATE core/include core/src third_party/cjson)
target_compile_definitions(fc-oci-pull PRIVATE CJSON_NESTING_LIMIT=32)
target_link_libraries(fc-oci-pull PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto Threads::Threads)

# Fuzzers (clang with libFuzzer): cmake -DFC_FUZZ=ON -DCMAKE_C_COMPILER=clang ...
# then tests/fuzz.sh runs them. Each one compiles the sources it tests, with
# the sanitizers on.
option(FC_FUZZ "Build the libFuzzer targets" OFF)
if(FC_FUZZ)
    set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1)
    add_compile_options(-Wno-deprecated-declarations)
    set(CJSON third_party/cjson/cJSON.c)
    function(fc_fuzzer name)
        add_executable(${name} tests/fuzz/${name}.c ${ARGN})
        target_include_directories(${name} PRIVATE core/include core/src third_party/cjson
                                   ${WAMR_ROOT_DIR}/core/iwasm/include)
        target_compile_options(${name} PRIVATE ${FUZZ_FLAGS})
        target_compile_definitions(${name} PRIVATE CJSON_NESTING_LIMIT=32)
        target_link_options(${name} PRIVATE ${FUZZ_FLAGS})
        target_link_libraries(${name} PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto Threads::Threads)
    endfunction()
    fc_fuzzer(fuzz_http ${CJSON})
    fc_fuzzer(fuzz_oci port/host/fc_port_host.c core/src/fc_log.c ${CJSON} ${DELTA_SRCS})
    fc_fuzzer(fuzz_delta ${DELTA_SRCS})
    fc_fuzzer(fuzz_console port/host/fc_port_host.c core/src/fc_log.c ${CJSON})
    fc_fuzzer(fuzz_agent core/src/fc_http.c core/src/fc_console.c core/src/fc_identity.c core/src/fc_apps.c core/src/fc_appapi.c core/src/fc_board.c core/src/fc_resources.c core/src/fc_log.c core/src/fc_metrics.c core/src/fc_mqtt.c port/host/fc_port_host.c ${CJSON})
    target_link_libraries(fuzz_agent PRIVATE vmlib)
endif()

# Unit tests: cmake -DFC_UNIT=ON ..., then ctest. Each test is one executable
# that includes the source it tests and runs on the in-memory port
# (tests/unit/port_stub.c). FC_SANITIZE=ON adds AddressSanitizer and UBSan.
option(FC_UNIT "Build the unit tests" OFF)
option(FC_SANITIZE "Build the unit tests with sanitizers" OFF)
if(FC_UNIT)
    enable_testing()
    function(fc_unit name)
        add_executable(${name} tests/unit/${name}.c tests/unit/port_stub.c ${ARGN})
        target_include_directories(${name} PRIVATE core/include core/src third_party/cjson tests/unit
                                   ${WAMR_ROOT_DIR}/core/iwasm/include)
        target_compile_definitions(${name} PRIVATE CJSON_NESTING_LIMIT=32)
        target_compile_options(${name} PRIVATE -Wno-unused-function -Wno-deprecated-declarations)
        if(FC_SANITIZE)
            target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -g)
            target_link_options(${name} PRIVATE -fsanitize=address,undefined)
        endif()
        target_link_libraries(${name} PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto Threads::Threads)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    fc_unit(test_http third_party/cjson/cJSON.c)
    fc_unit(test_oci third_party/cjson/cJSON.c ${DELTA_SRCS})
    fc_unit(test_identity core/src/fc_identity.c core/src/fc_http.c third_party/cjson/cJSON.c)
    fc_unit(test_agent core/src/fc_identity.c core/src/fc_http.c core/src/fc_resources.c core/src/fc_metrics.c core/src/fc_mqtt.c third_party/cjson/cJSON.c)
    fc_unit(test_resources third_party/cjson/cJSON.c)
    fc_unit(test_console third_party/cjson/cJSON.c core/src/fc_log.c)
    fc_unit(test_apps third_party/cjson/cJSON.c core/src/fc_appapi.c core/src/fc_board.c core/src/fc_metrics.c core/src/fc_http.c core/src/fc_mqtt.c)
    target_link_libraries(test_apps PRIVATE vmlib)
    fc_unit(test_threads third_party/cjson/cJSON.c)
    fc_unit(test_metrics third_party/cjson/cJSON.c)
    fc_unit(test_fatal third_party/cjson/cJSON.c)
    fc_unit(test_log third_party/cjson/cJSON.c)
    fc_unit(test_mqtt core/src/fc_http.c third_party/cjson/cJSON.c)
    # The MQTT client against a real broker (tests/mqtt-broker.sh), not in ctest.
    add_executable(mqtt-broker-check EXCLUDE_FROM_ALL tests/unit/mqtt_broker_check.c tests/unit/port_stub.c
                   core/src/fc_mqtt.c core/src/fc_http.c third_party/cjson/cJSON.c)
    target_include_directories(mqtt-broker-check PRIVATE core/include core/src third_party/cjson tests/unit)
    target_compile_definitions(mqtt-broker-check PRIVATE CJSON_NESTING_LIMIT=32)
    target_compile_options(mqtt-broker-check PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(mqtt-broker-check PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto Threads::Threads)
endif()
