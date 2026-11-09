# Agent core (POSIX + mbedTLS + cJSON) and the ESP-IDF port, shared with the host build.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
    SRCS ${ROOT}/core/src/fc_agent.c
         ${ROOT}/core/src/fc_http.c
         ${ROOT}/core/src/fc_identity.c
         ${ROOT}/core/src/fc_oci.c
         ${ROOT}/core/src/fc_apps.c
         ${ROOT}/port/espidf/fc_port_espidf.c
    INCLUDE_DIRS ${ROOT}/core/include
    PRIV_INCLUDE_DIRS ${ROOT}/core/src
    REQUIRES mbedtls pthread wamr json
    PRIV_REQUIRES soc nvs_flash bootloader_support esp_wifi esp_netif esp_driver_tsens esp_app_format esp_timer app_update efuse)
# cJSON is ESP-IDF's (component json), built with the nesting limit set in
# firmware/CMakeLists.txt; the host build and the fuzzers use core/third_party.
# The firmware's security variant, read from an update before it is installed
# (fc_port_espidf.c): unreferenced, so kept explicitly.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fc_fw_desc")
