# SPDX-License-Identifier: GPL-3.0-or-later
#
# piBrick CM5 kernel drivers and device-tree overlay
#

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
DTC  ?= dtc

DTSDIR  := dts
DTS     := $(DTSDIR)/pibrick.dts
DTBO    := $(DTSDIR)/pibrick.dtbo

obj-m += panel/panel-pibrick.o
obj-m += power/pibrick-battery.o
obj-m += power/pibrick-charger.o
obj-m += accel/pibrick-mma8451.o

.PHONY: all modules dtbo clean install uninstall

all: modules dtbo

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	$(MAKE) -C touch/hyn_ts KDIR=$(KDIR) modules

dtbo: $(DTBO)

$(DTBO): $(DTS)
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C touch/hyn_ts KDIR=$(KDIR) clean
	rm -f $(DTBO)

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	$(MAKE) -C touch/hyn_ts KDIR=$(KDIR) modules_install
	install -D -m 0644 \
		$(DTBO) \
		/boot/firmware/overlays/pibrick.dtbo
	depmod -a

uninstall:
	rm -f /boot/firmware/overlays/pibrick.dtbo
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C touch/hyn_ts KDIR=$(KDIR) clean
	depmod -a
