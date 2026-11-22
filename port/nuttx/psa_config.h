#pragma once
#include <nuttx/config.h>
#ifndef CONFIG_DEV_URANDOM_ARCH
#error "TLS requires the hardware random device"
#endif
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_PLATFORM_MEMORY
#ifdef CONFIG_FLIGHTCTL_NRF7002DK
/* The network core exports its hardware RNG at this path over RPMsg. */
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"
#endif
#if defined(CONFIG_ARCH_CHIP_ESP32C3) || defined(CONFIG_ARCH_CHIP_ESP32C5) || \
    defined(CONFIG_ESP32_IGNORE_CHIP_REVISION_CHECK)
#define MBEDTLS_AES_ROM_TABLES
#endif
