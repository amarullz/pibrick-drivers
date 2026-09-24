# SPDX-License-Identifier: GPL-3.0-or-later

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m += hyn_ts.o

hyn_ts-objs += hyn_core.o
hyn_ts-objs += hyn_lib/hyn_i2c.o
hyn_ts-objs += hyn_lib/hyn_spi.o
hyn_ts-objs += hyn_lib/hyn_ts_ext.o
hyn_ts-objs += hyn_lib/hyn_fs_node.o
hyn_ts-objs += hyn_lib/hyn_tool.o
hyn_ts-objs += hyn_lib/hyn_gesture.o
hyn_ts-objs += hyn_lib/hyn_prox.o

hyn_ts-objs += hyn_chips/hyn_cst66xx.o
hyn_ts-objs += hyn_chips/hyn_cst3240.o
hyn_ts-objs += hyn_chips/hyn_cst92xx.o
hyn_ts-objs += hyn_chips/hyn_cst3xx.o
hyn_ts-objs += hyn_chips/hyn_cst7xx.o
hyn_ts-objs += hyn_chips/hyn_cst8xxT.o
hyn_ts-objs += hyn_chips/hyn_cst226se.o
hyn_ts-objs += hyn_chips/hyn_cst36xxes.o


.PHONY: all modules clean


all: modules


modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules


clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
