# Enrollment credentials come from `flightctl certificate request --signer=enrollment`
# (see deploy/README). They are embedded at build time and never committed.
set(SECRETS ${CMAKE_CURRENT_LIST_DIR}/../../.secrets)
idf_component_register(
    SRCS main.c alarm.c led.c
    PRIV_REQUIRES fc_core wamr nvs_flash esp_wifi esp_netif esp_event app_update esp_driver_rmt esp_driver_gpio esp_timer esp_app_format
    EMBED_TXTFILES ${SECRETS}/ca.crt ${SECRETS}/enroll.crt ${SECRETS}/enroll.key)
