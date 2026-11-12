# Credentials are not built in: the device reads them from its fc_creds
# partition (deploy/provision.sh), so one image serves every installation.
idf_component_register(
    SRCS main.c alarm.c led.c
    PRIV_REQUIRES fc_core wamr nvs_flash nvs_sec_provider esp_wifi esp_netif esp_event app_update esp_driver_rmt esp_driver_gpio esp_timer esp_app_format)
