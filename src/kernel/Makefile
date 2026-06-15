# SPDX-License-Identifier: GPL-2.0-only
#
# MiniMem kernel module Makefile
#
# Build with:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#
# Load with:
#   sudo insmod minimem.ko
#
# Check stats:
#   cat /sys/kernel/minimem/pages_compressed
#
# Unload with:
#   sudo rmmod minimem

obj-m += minimem.o

minimem-y := minimem_main.o

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

.PHONY: all clean install