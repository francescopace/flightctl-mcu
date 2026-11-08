# Enrollment credentials come from `flightctl certificate request --signer=enrollment`
# (see deploy/README). They are embedded at build time and never committed.
# The image signing key (deploy/signing/keygen.sh) is embedded when present:
# then the device only installs firmware and apps signed with it.
set(SECRETS ${CMAKE_CURRENT_LIST_DIR}/../../.secrets)
if(DEFINED ENV{FC_SECRETS_DIR}) # tests: credentials of a throwaway cluster
    set(SECRETS $ENV{FC_SECRETS_DIR})
endif()
set(EMBED ${SECRETS}/ca.crt ${SECRETS}/enroll.crt ${SECRETS}/enroll.key)
if(EXISTS ${SECRETS}/signing.pub)
    list(APPEND EMBED ${SECRETS}/signing.pub)
endif()
idf_component_register(
    SRCS main.c alarm.c led.c
    PRIV_REQUIRES fc_core wamr nvs_flash esp_wifi esp_netif esp_event app_update esp_driver_rmt esp_driver_gpio esp_timer esp_app_format
    EMBED_TXTFILES ${EMBED})
if(EXISTS ${SECRETS}/signing.pub)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FC_SIGNING_KEY=1)
endif()
