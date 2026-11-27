# Out-of-tree build driver for one na51089 sensor directory.
#
#   make KERNELDIR=/path/to/configured/linux-4.19.91 \
#        CROSS_COMPILE=arm-ca9-linux-uclibcgnueabihf-
#
# KERNELDIR must have been through `make modules_prepare` (or a full build)
# with an na51089 config: the drivers include <plat/nvt-gpio.h> from the
# Novatek kernel tree.

ARCH ?= arm
export ARCH

ifeq ($(KERNELDIR),)
$(error KERNELDIR must point at a prepared na51089 kernel tree)
endif

all modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean

.PHONY: all modules clean
