/* Boards with no application pin map still run portable WASM workloads. */
#include <nuttx/config.h>
#include "fc_agent.h"
#include "fc_port.h"
#include "fc_hw_nuttx.h"

const fc_hw_t fc_nuttx_hardware = {
    .board = CONFIG_ARCH_CHIP, .firmware = FC_AGENT_VERSION,
#if defined(CONFIG_ESPRESSIF_TEMP) || defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W) || \
    defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI) || defined(CONFIG_FLIGHTCTL_NRF7002DK)
    .sensor_read = fc_nuttx_sensor,
    .sensor_subscribe = fc_nuttx_subscribe,
    .sensor_unsubscribe = fc_nuttx_unsubscribe,
#endif
};

size_t fc_port_coredump_size(void) { return 0; }
int fc_port_coredump_read(size_t offset, void *buf, size_t len) { return -1; }
void fc_port_coredump_erase(void) {}
