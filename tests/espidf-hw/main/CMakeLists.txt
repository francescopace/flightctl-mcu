set(port "${CMAKE_CURRENT_LIST_DIR}/../../../port/espidf/main")
if(IDF_TARGET STREQUAL "esp32s3")
    set(fixture wiring.c adc.c)
elseif(IDF_TARGET STREQUAL "esp32c3")
    set(fixture adc.c)
endif()
idf_component_register(
    SRCS temperature.c ${fixture} "${port}/../fc_temperature.c" "${port}/hw.c" "${port}/led.c" "${port}/hw_bus.c"
    INCLUDE_DIRS "${port}" "${port}/.." "../../../core/include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart
                  esp_driver_ledc esp_driver_rmt esp_driver_tsens pthread esp_adc esp_app_format)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_FC_LED_GPIO=-1)
