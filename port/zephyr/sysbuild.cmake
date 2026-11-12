# Builds with a bootloader (sysbuild). A board's variant started by MCUboot is
# that board, plus what boards/<board>_mcuboot.conf adds: Zephyr reads only
# the variant's file, so the board's own is added here.
# (The qualifiers come with their leading "/" or without it, by Zephyr version.)
if("${BOARD}/${BOARD_QUALIFIERS}" MATCHES "^rpi_pico//?rp2040/w/mcuboot$")
    set(${DEFAULT_IMAGE}_EXTRA_CONF_FILE ${APP_DIR}/boards/rpi_pico_rp2040_w.conf CACHE INTERNAL "")
endif()
