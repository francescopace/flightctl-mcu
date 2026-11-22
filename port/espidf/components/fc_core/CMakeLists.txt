# Agent core (POSIX + mbedTLS + cJSON) and the ESP-IDF port, shared with the host build.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
idf_component_register(
    SRCS ${ROOT}/core/src/fc_agent.c
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
         ${ROOT}/core/src/fc_prov.c
         ${ROOT}/third_party/cjson/cJSON.c
         ${ROOT}/port/espidf/fc_port_espidf.c
         ${ROOT}/port/espidf/fc_temperature.c
         ${ROOT}/port/espidf/fc_wasm_mem.c
         ${ROOT}/port/espidf/fc_fatal_espidf.c
    INCLUDE_DIRS ${ROOT}/core/include ${ROOT}/third_party/cjson
    PRIV_INCLUDE_DIRS ${ROOT}/core/src
    REQUIRES mbedtls pthread wamr
    PRIV_REQUIRES lwip soc nvs_flash bootloader_support esp_wifi esp_netif esp_driver_tsens esp_app_format esp_timer app_update efuse esp_system spi_flash espcoredump)
# cJSON is the core's copy, as in the host build (ESP-IDF 6 no longer has
# one), with the agent's nesting limit: cJSON parses recursively, and its
# default of 1000 levels overflowed the agent's 24 KB thread stacks.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CJSON_NESTING_LIMIT=32)
set_property(SOURCE ${ROOT}/core/src/fc_apps.c APPEND PROPERTY COMPILE_DEFINITIONS FC_APP_STATE_LOG_BUFFER_ONLY)
# The device's log in RAM that a restart keeps (fc_log.h).
target_compile_definitions(${COMPONENT_LIB} PRIVATE FC_LOG_KEEP=1)
if(CONFIG_FC_CONSOLE_CRASH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FC_CONSOLE_CRASH=1)
endif()
# The firmware's security variant, read from an update before it is installed
# (fc_port_espidf.c): unreferenced, so kept explicitly.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fc_fw_desc")
if(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
    target_sources(${COMPONENT_LIB} PRIVATE ${ROOT}/port/espidf/fc_tls_record.c)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FC_IDF_DYNAMIC_TLS_RX=1)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_mbedtls_add_rx_buffer" "-u __wrap_esp_mbedtls_add_rx_buffer")
endif()
# WASM apps' linear memories in blocks of their own size (fc_wasm_mem.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=os_mmap" "-u __wrap_os_mmap"
                      "-Wl,--wrap=os_munmap" "-u __wrap_os_munmap" "-Wl,--wrap=os_mremap" "-u __wrap_os_mremap")
# What a crash was, kept across the restart (fc_fatal_espidf.c): ESP-IDF's
# panic handler wrapped, as its own panic tests do.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler" "-u __wrap_esp_panic_handler")
