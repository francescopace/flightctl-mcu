if("${BOARD}/${BOARD_QUALIFIERS}" MATCHES "^rpi_pico2//?rp2350a/m33_0/w/mcuboot")
    set(${DEFAULT_IMAGE}_EXTRA_CONF_FILE ${APP_DIR}/boards/rpi_pico2_rp2350a_m33_0_w.conf CACHE INTERNAL "")
    set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/boards/rpi_pico2_ota_slots.dtsi CACHE INTERNAL "")
endif()

if("${BOARD}/${BOARD_QUALIFIERS}" MATCHES "^nrf7002dk//?nrf5340/cpuapp")
    set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${APP_DIR}/sysbuild/nrf7002dk_mcuboot.overlay CACHE INTERNAL "")
    list(APPEND mcuboot_EXTRA_CONF_FILE ${APP_DIR}/sysbuild/nrf7002dk_mcuboot.conf)
    set(mcuboot_EXTRA_CONF_FILE "${mcuboot_EXTRA_CONF_FILE}" CACHE INTERNAL "")
    ExternalZephyrProject_Add(
        APPLICATION hci_ipc
        SOURCE_DIR ${APP_DIR}/nrf5340/hci_ipc
        BOARD nrf7002dk/nrf5340/cpunet
    )
endif()
