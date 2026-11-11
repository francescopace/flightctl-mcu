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

# detools (third_party/fetch-detools.sh) applies firmware deltas: its
# sequential patches with heatshrink only, and its allocations through
# fc_delta.c, which caps them. -fwrapv: it does arithmetic on sizes read from
# the patch in plain ints, and a patch is not trusted; wrapping is harmless
# there (fc_delta.c checks every read and write), undefined behavior is not.
# The same settings as the firmware build.
set(DETOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/detools/c)
set(DELTA_SRCS core/src/fc_delta.c ${DETOOLS_DIR}/detools.c ${DETOOLS_DIR}/heatshrink/heatshrink_decoder.c)
set(DETOOLS_DEFS DETOOLS_CONFIG_FILE_IO=0 DETOOLS_CONFIG_COMPRESSION_NONE=0 DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0 HEATSHRINK_DYNAMIC_ALLOC=1)
set_source_files_properties(${DELTA_SRCS} PROPERTIES
    COMPILE_DEFINITIONS "${DETOOLS_DEFS}"
    INCLUDE_DIRECTORIES "${DETOOLS_DIR};${DETOOLS_DIR}/heatshrink")
set_source_files_properties(${DETOOLS_DIR}/detools.c ${DETOOLS_DIR}/heatshrink/heatshrink_decoder.c PROPERTIES
    COMPILE_DEFINITIONS "${DETOOLS_DEFS};malloc=fc_delta_alloc;free=fc_delta_free"
    COMPILE_OPTIONS "-w;-fwrapv")

add_library(fc_core STATIC
    core/src/fc_agent.c
    core/src/fc_http.c
    core/src/fc_identity.c
    core/src/fc_oci.c
    core/src/fc_apps.c
    core/third_party/cjson/cJSON.c
    ${DELTA_SRCS}
)
target_include_directories(fc_core PUBLIC core/include core/third_party/cjson PRIVATE core/src)
# Same cJSON nesting limit as the firmware (firmware/components/fc_core).
target_compile_definitions(fc_core PUBLIC CJSON_NESTING_LIMIT=32)
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
        target_compile_definitions(${name} PRIVATE CJSON_NESTING_LIMIT=32)
        target_link_options(${name} PRIVATE ${FUZZ_FLAGS})
        target_link_libraries(${name} PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto Threads::Threads)
    endfunction()
    fc_fuzzer(fuzz_http ${CJSON})
    fc_fuzzer(fuzz_oci port/host/fc_port_host.c ${CJSON} ${DELTA_SRCS})
    fc_fuzzer(fuzz_delta ${DELTA_SRCS})
    fc_fuzzer(fuzz_agent core/src/fc_http.c core/src/fc_identity.c core/src/fc_apps.c port/host/fc_port_host.c ${CJSON})
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
        target_include_directories(${name} PRIVATE core/include core/src core/third_party/cjson tests/unit
                                   ${WAMR_ROOT_DIR}/core/iwasm/include)
        target_compile_definitions(${name} PRIVATE CJSON_NESTING_LIMIT=32)
        target_compile_options(${name} PRIVATE -Wno-unused-function -Wno-deprecated-declarations)
        if(FC_SANITIZE)
            target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -g)
            target_link_options(${name} PRIVATE -fsanitize=address,undefined)
        endif()
        target_link_libraries(${name} PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto Threads::Threads)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    fc_unit(test_http core/third_party/cjson/cJSON.c)
    fc_unit(test_oci core/third_party/cjson/cJSON.c ${DELTA_SRCS})
    fc_unit(test_identity core/src/fc_identity.c core/src/fc_http.c core/third_party/cjson/cJSON.c)
    fc_unit(test_agent core/src/fc_identity.c core/src/fc_http.c core/third_party/cjson/cJSON.c)
endif()
