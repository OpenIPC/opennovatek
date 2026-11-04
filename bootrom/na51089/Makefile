CROSS_COMPILE ?= arm-none-eabi-
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJDUMP := $(CROSS_COMPILE)objdump
OBJCOPY := $(CROSS_COMPILE)objcopy

# -Os: gcc's -O2 output overruns the 32 KiB ROM by about 4 KiB and runs
# into the exported table pinned at 0x7FB8; -Os keeps the layout's shape.
CFLAGS  := -mcpu=cortex-a9 -marm -nostdlib -ffreestanding \
           -fno-builtin -fno-stack-protector -fno-pic \
           -Wall -Wextra -Werror -Wno-unused-parameter \
           -Os -g \
           -I$(CURDIR)/re -I$(CURDIR)/../common

LDFLAGS := -nostdlib -Wl,--build-id=none -T re/link.ld

# start.o must stay first: link.ld places its .text at 0.
OBJS := re/start.o \
        re/lib.o re/timer.o re/boot.o re/loader.o re/secure.o \
        re/console.o re/uart_boot.o re/eth.o re/net.o \
        re/sdio.o re/sd.o re/emmc.o re/flash.o \
        re/usb.o re/usb_msc.o re/lz.o re/lzma.o \
        re/export.o

HDRS := $(wildcard re/*.h)

.PHONY: all build disasm verify clean

all: build

build: re.elf

re.elf: $(OBJS) re/link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

re/%.o: re/%.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

re/%.o: re/%.S $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

disasm:
	@../scripts/disasm.sh na51089

verify:
	@../scripts/verify.sh na51089

clean:
	rm -f $(OBJS) re.elf
