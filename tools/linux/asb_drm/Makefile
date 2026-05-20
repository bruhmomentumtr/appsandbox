# Developer convenience wrapper around Kbuild. DKMS builds use Kbuild
# directly; this is for `make` / `make install` during local iteration
# on the VM build host.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install into the running kernel's tree and load.
install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
	@echo "asb_drm built and installed."
	@echo "To activate: modprobe asb_drm"

uninstall:
	-rmmod asb_drm 2>/dev/null
	rm -f /lib/modules/$(shell uname -r)/extra/asb_drm.ko*
	depmod -a

reload: install
	-rmmod asb_drm 2>/dev/null
	modprobe asb_drm

.PHONY: all clean install uninstall reload
