# Agent core (POSIX + mbedTLS + cJSON) and the ESP-IDF port, shared with the host build.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
# detools (third_party/fetch-detools.sh): applies firmware deltas (fc_delta.c).
set(DETOOLS ${ROOT}/third_party/detools/c)
idf_component_register(
    SRCS ${ROOT}/core/src/fc_agent.c
         ${ROOT}/core/src/fc_http.c
         ${ROOT}/core/src/fc_identity.c
         ${ROOT}/core/src/fc_oci.c
         ${ROOT}/core/src/fc_apps.c
         ${ROOT}/core/src/fc_delta.c
         ${ROOT}/core/third_party/cjson/cJSON.c
         ${DETOOLS}/detools.c
         ${DETOOLS}/heatshrink/heatshrink_decoder.c
         ${ROOT}/port/espidf/fc_port_espidf.c
    INCLUDE_DIRS ${ROOT}/core/include ${ROOT}/core/third_party/cjson
    PRIV_INCLUDE_DIRS ${ROOT}/core/src ${DETOOLS} ${DETOOLS}/heatshrink
    REQUIRES mbedtls pthread wamr
    PRIV_REQUIRES soc nvs_flash bootloader_support esp_wifi esp_netif esp_driver_tsens esp_app_format esp_timer app_update efuse)
# cJSON is the core's copy, as in the host build (ESP-IDF 6 no longer has
# one), with the agent's nesting limit: cJSON parses recursively, and its
# default of 1000 levels overflowed the agent's 24 KB thread stacks
# (RESULTS.md, problem 14).
target_compile_definitions(${COMPONENT_LIB} PUBLIC CJSON_NESTING_LIMIT=32)
# The firmware's security variant, read from an update before it is installed
# (fc_port_espidf.c): unreferenced, so kept explicitly.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u fc_fw_desc")
# detools as in the host build (CMakeLists.txt at the root): sequential patches
# with heatshrink only, allocations through fc_delta.c, which caps them, and
# wrapping arithmetic, as it computes with sizes read from a patch.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DETOOLS_CONFIG_FILE_IO=0 DETOOLS_CONFIG_COMPRESSION_NONE=0 DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0 HEATSHRINK_DYNAMIC_ALLOC=1)
set_source_files_properties(${DETOOLS}/detools.c ${DETOOLS}/heatshrink/heatshrink_decoder.c PROPERTIES
    COMPILE_DEFINITIONS "malloc=fc_delta_alloc;free=fc_delta_free"
    COMPILE_OPTIONS "-w;-fwrapv")
