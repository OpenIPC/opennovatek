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

all modules:
	@test -n "$(KERNELDIR)" || \
		{ echo "KERNELDIR must point at a prepared na51089 kernel tree" >&2; exit 1; }
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

# Without a kernel tree to hand, remove what kbuild leaves in this directory,
# plus any sources the directory's Kbuild generates (GENERATED).
clean:
ifneq ($(KERNELDIR),)
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean
else
	rm -rf *.o *.ko *.mod *.mod.c .*.cmd .tmp_versions modules.order Module.symvers $(GENERATED)
endif

.PHONY: all modules clean
