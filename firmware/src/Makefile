all: flash

TARGET     := noknok_usb_bootloader
TARGET_MCU := CH32V203

# ch32fun path on RPi4
CH32FUN_DIR := /home/noknok/dev/ch32fun

# Use our custom 8 KB-bootloader linker instead of the generated full-flash one.
LINKER_SCRIPT := bootloader.ld

ADDITIONAL_C_FILES :=

include $(CH32FUN_DIR)/ch32fun/ch32fun.mk

# Point to the USBD (FSDEV) USB device library in ch32fun extralibs
CFLAGS += -I$(CH32FUN_DIR)/extralibs -include stdio.h
EXTRA_C_FILES += $(CH32FUN_DIR)/extralibs/usbd.c

flash: cv_flash
clean: cv_clean
