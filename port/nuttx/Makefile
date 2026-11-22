include $(APPDIR)/Make.defs
PROGNAME = flightctl
PRIORITY = 100
STACKSIZE = 18432
ifneq ($(filter y,$(CONFIG_ARCH_CHIP_ESP32C3) $(CONFIG_ARCH_CHIP_ESP32C5) $(CONFIG_ESP32_IGNORE_CHIP_REVISION_CHECK)),)
STACKSIZE = 14336
endif
MAINSRC = main.c
VPATH += $(FC_ROOT)/port/nuttx
arch_src := $(TOPDIR)/arch/$(subst ",,$(CONFIG_ARCH))/src
CFLAGS += -I$(FC_ROOT)/core/include -I$(FC_ROOT)/third_party/cjson -I$(arch_src)/chip -I$(arch_src)/common
export FC_NUTTX_HAL = $(arch_src)/chip/esp-hal-3rdparty
include $(APPDIR)/Application.mk

# Build with the same compiler, headers and ABI as the NSH built-in.
# The NuttX linker groups all archives in apps/staging with libapps.
.PHONY: fc-libraries
all:: fc-libraries
fc-libraries:
	$(Q) cmake -S $(FC_ROOT)/port/nuttx -B libs -G Ninja \
	    -DWAMR_ROOT_DIR="$(or $(FC_WAMR),$(FC_ROOT)/third_party/wamr)" \
	    -DFC_NUTTX_CHIP=$(CONFIG_ARCH_CHIP) -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
	    -DCMAKE_C_COMPILER="$(CC)" -DCMAKE_ASM_COMPILER="$(CC)" \
	    -DCMAKE_C_FLAGS="$(CFLAGS)" -DCMAKE_ASM_FLAGS="$(AFLAGS)" \
	    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$(APPDIR)/staging"
	$(Q) cmake --build libs --parallel 6
