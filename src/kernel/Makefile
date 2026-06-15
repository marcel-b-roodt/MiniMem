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
#   cat /sys/kernel/minimem/map_entries
#
# Unload with:
#   sudo rmmod minimem

obj-m += minimem.o

minimem-y := minimem_main.o minimem_compress.o minimem_map.o

# Algorithm library sources compiled for kernel context.
# These use only memcpy/memset and stack variables (no malloc).
# The KERNEL define adjusts any libc-specific includes.
minimem-y += ../lib/compressors/same_page.o
minimem-y += ../lib/compressors/bdi.o
minimem-y += ../lib/compressors/wkdm.o
minimem-y += ../lib/compressors/wkdm64.o
minimem-y += ../lib/compressors/block_class.o

# LZ4 vendor source (kernel-compatible)
minimem-y += ../lib/vendor/lz4/lz4.o
minimem-y += ../lib/vendor/lz4/lz4hc.o

ccflags-y := -I$(src)/.. -DMINIMEM_KERNEL -include minimem_compat.h

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

.PHONY: all clean install