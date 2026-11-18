# The Zephyr port's hardware for apps (port/zephyr/fc_hw_zephyr.c) against
# native_sim's emulated drivers: GPIO, ADC, PWM, I2C with an AT24 EEPROM,
# SPI with a loopback device, UART with loopback. A ztest suite, built and
# run with tests/zephyr-hw.sh.
cmake_minimum_required(VERSION 3.20)
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
list(APPEND DTS_ROOT ${ROOT}/port/zephyr ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fc_zephyr_hw)
target_compile_definitions(app PRIVATE FC_TEST_RS485=1)
target_include_directories(app PRIVATE ${ROOT}/core/include ${ROOT}/third_party/cjson)
target_sources(app PRIVATE src/main.c src/spi_loopback.c ${ROOT}/port/zephyr/fc_hw_zephyr.c ${ROOT}/third_party/cjson/cJSON.c)
