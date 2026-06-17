# SPDX-License-Identifier: GPL-2.0-only
#
# MiniMem kernel module Makefile — DKMS-compatible
#
# Called by DKMS: make -C /lib/modules/$KVER/build M=$PWD modules
# Also works standalone: make   (builds for running kernel)

obj-m += minimem.o

minimem-y := minimem_main.o minimem_compress.o minimem_map.o minimem_zswap.o \
	     minimem_parallel.o minimem_fault.o minimem_shrinker.o \
	     minimem_scanner.o minimem_hook.o minimem_proc_stats.o

minimem-y += lib/compressors/same_page.o
minimem-y += lib/compressors/bdi.o
minimem-y += lib/compressors/wkdm.o
minimem-y += lib/compressors/wkdm64.o
minimem-y += lib/compressors/block_class.o
minimem-y += lib/compressors/lz4_wrap.o
minimem-y += lib/compressors/delta.o
minimem-y += lib/advisor.o

ccflags-y := -I$(src)/include -I$(src) -I$(src)/lib -DMINIMEM_KERNEL

KVER ?= $(shell uname -r)
KERNELDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: all clean install