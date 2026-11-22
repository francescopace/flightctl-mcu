#pragma once

#include <nuttx/config.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_FLIGHTCTL_NRF7002DK)
#define FC_ARM_PLATFORM "nuttx-nrf7002dk"
#define FC_PRIMARY 0x00010000u
#define FC_SLOT_SIZE 0xec000u
#define FC_HEADER_SIZE 0x200u
#define FC_ERASE_SIZE 4096u
#elif defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI)
#ifdef CONFIG_STM32_ETHMAC
#define FC_ARM_PLATFORM "nuttx-nucleo-f767zi-eth"
#else
#define FC_ARM_PLATFORM "nuttx-nucleo-f767zi"
#endif
#define FC_PRIMARY 0x08040000u
#define FC_SLOT_SIZE 0xc0000u
#define FC_HEADER_SIZE 0x400u
#define FC_ERASE_SIZE 0x40000u
#elif defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W)
#define FC_ARM_PLATFORM "nuttx-pico2w"
#define FC_PRIMARY 0x10020000u
#define FC_SLOT_SIZE 0x170000u
#define FC_HEADER_SIZE 0x200u
#define FC_ERASE_SIZE 4096u
#endif

#ifdef FC_ARM_PLATFORM
/* Offsets below are relative to the secondary slot. The data and trailer are
 * programmed a page at a time, including on the two external NOR devices. */
#define FC_PAGE_SIZE 256u
int fc_arm_secondary_read(size_t off, void *dst, size_t len);
int fc_arm_secondary_erase(size_t off);
int fc_arm_secondary_program(size_t off, const uint8_t page[FC_PAGE_SIZE]);
int fc_arm_confirm_flag(size_t off);
#endif
