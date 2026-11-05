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

add_library(fc_core STATIC
    core/src/fc_agent.c
    core/src/fc_http.c
    core/src/fc_identity.c
    core/src/fc_oci.c
    core/third_party/cjson/cJSON.c
)
target_include_directories(fc_core PUBLIC core/include core/third_party/cjson PRIVATE core/src)
target_link_libraries(fc_core PUBLIC MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto Threads::Threads)

add_executable(fc-host-agent host/main.c port/host/fc_port_host.c)
target_link_libraries(fc-host-agent PRIVATE fc_core)
