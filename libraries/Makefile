# Build every library/driver directory for one CHIPARCH:
#
#   make CHIPARCH=na51089 KERNELDIR=/path/to/linux \
#        CROSS_COMPILE=arm-ca9-linux-uclibcgnueabihf-
#
# Laid out like openhisilicon's libraries/: sensor/<chiparch>/<vendor>_<model>/.
# One difference: Novatek sensor drivers are kernel modules (nvt_sen_*.ko,
# registered with the HDAL ctl_sen framework), not userspace libsns_*.so, so
# a sensor build needs a prepared kernel tree in KERNELDIR.

MAKEFILES = $(shell find . -mindepth 2 -maxdepth 4 -type f -name Makefile)
SUBDIRS   = $(filter-out ./,$(dir $(MAKEFILES)))

ifeq ($(CHIPARCH),)
    $(error CHIPARCH must be set (supported: na51089))
endif

# na51089 (NT98560/1/2/6): sensor drivers from SDK source.
ifeq ($(CHIPARCH),na51089)
    SUBDIRS := $(filter ./sensor/na51089/%,$(SUBDIRS))
else
    $(error unsupported CHIPARCH '$(CHIPARCH)' (supported: na51089))
endif

all:
	@failed=""; \
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir CHIPARCH=$(CHIPARCH) all || failed="$$failed $$dir"; \
	done; \
	if [ -n "$$failed" ]; then \
		echo; \
		echo "Build failed for:" >&2; \
		for d in $$failed; do echo "  $$d" >&2; done; \
		exit 1; \
	fi

clean:
	@failed=""; \
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean || failed="$$failed $$dir"; \
	done; \
	if [ -n "$$failed" ]; then \
		echo "Clean failed for:$$failed" >&2; \
		exit 1; \
	fi

.PHONY: all clean
