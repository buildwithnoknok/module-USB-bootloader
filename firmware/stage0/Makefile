# noknok USB stage-0 (CH32V203, layout 4) - standalone build, no ch32fun.c (DEV-237)
#   make build   -> noknok_usb_stage0.bin  (must fit 1 KB)
#   make size    -> report the size against the frozen budget
# ch32fun.h is used for register definitions only.

BUDGET   = 1024
CH32FUN_DIR ?= /home/noknok/dev/ch32fun
CH32FUN  = $(CH32FUN_DIR)/ch32fun
TARGET   = noknok_usb_stage0
ARCH     = -march=rv32imac_zicsr -mabi=ilp32 -DCH32V20x=1 -DCH32V20x_D6

PREFIX   = riscv64-unknown-elf
CC       = $(PREFIX)-gcc
OBJCOPY  = $(PREFIX)-objcopy
OBJDUMP  = $(PREFIX)-objdump

CFLAGS = -g -Os -flto \
         -ffunction-sections -fdata-sections -fmessage-length=0 \
         -msmall-data-limit=8 -fno-tree-loop-distribute-patterns \
         $(ARCH) \
         -static-libgcc -nostdlib -Wall \
         -I. -I$(CH32FUN) -I$(CH32FUN_DIR)/extralibs -I/usr/include/newlib

LDFLAGS = -T stage0.ld -Wl,--gc-sections \
          -Wl,--print-memory-usage -Wl,-Map=$(TARGET).map -lgcc

SRCS = start.S noknok_usb_stage0.c

all   : build
build : $(TARGET).bin

$(TARGET).elf : $(SRCS) stage0.ld
	$(CC) -o $@ $(SRCS) $(CFLAGS) $(LDFLAGS)

$(TARGET).bin : $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -S $< > $(TARGET).lst
	@$(MAKE) --no-print-directory size

size :
	@sz=`stat -c %s $(TARGET).bin`; free=`expr $(BUDGET) - $$sz`; \
	 echo "stage-0 (v203): $$sz B of $(BUDGET) B budget ($$free B free)"

clean :
	rm -f $(TARGET).elf $(TARGET).bin $(TARGET).lst $(TARGET).map

.PHONY : all build size clean
