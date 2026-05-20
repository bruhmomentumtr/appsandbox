# Out-of-tree builder for dxgkrnl. Builds against the running kernel by
# default; override KDIR for a specific kernel.
#
# Usage:
#   make            # build dxgkrnl.ko for $(uname -r)
#   make clean
#   make install    # copy to /lib/modules/$(uname -r)/updates/dxgkrnl/ + depmod
#
# Source is vendored from microsoft/WSL2-Linux-Kernel branch
# linux-msft-wsl-6.18.y, drivers/hv/dxgkrnl/. The Kbuild lives in src/
# alongside the source files so the kernel build system can find the
# .c objects via the standard out-of-tree pattern (M=$(PWD)/src).

KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)
INSTDIR ?= /lib/modules/$(shell uname -r)/updates/dxgkrnl

all:
	$(MAKE) -C $(KDIR) M=$(PWD)/src modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/src clean

install: all
	install -d $(INSTDIR)
	install -m 0644 src/dxgkrnl.ko $(INSTDIR)/
	depmod -a $(shell uname -r)

.PHONY: all clean install
