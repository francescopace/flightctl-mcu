# Agent core (POSIX + mbedTLS + cJSON) and the ESP-IDF port, shared with the host build.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(
    SRCS ${ROOT}/core/src/fc_agent.c
         ${ROOT}/core/src/fc_http.c
         ${ROOT}/core/src/fc_identity.c
         ${ROOT}/core/src/fc_oci.c
         ${ROOT}/core/src/fc_apps.c
         ${ROOT}/core/third_party/cjson/cJSON.c
         ${ROOT}/port/espidf/fc_port_espidf.c
    INCLUDE_DIRS ${ROOT}/core/include ${ROOT}/core/third_party/cjson
    PRIV_INCLUDE_DIRS ${ROOT}/core/src
    REQUIRES mbedtls pthread wamr
    PRIV_REQUIRES soc nvs_flash esp_wifi esp_netif esp_driver_tsens esp_app_format esp_timer app_update)
# cJSON parses recursively: its default 1000 levels of nesting overflow the
# agent's 24 KB thread stacks (a nested fleet config made devices crash in a
# loop). Real documents use fewer than 10.
target_compile_definitions(${COMPONENT_LIB} PUBLIC CJSON_NESTING_LIMIT=32)
