# SPDX-License-Identifier: GPL-2.0-only
#
# MiniMem kernel module Makefile
#
# Build from a path without spaces using the build-kmod.sh script:
#   ./build-kmod.sh
#
# Do NOT build directly with make if your source path contains spaces.
# The kernel build system cannot handle spaces in M= paths.

obj-m += minimem.o

minimem-y := minimem_main.o minimem_compress.o minimem_map.o minimem_zswap.o minimem_parallel.o minimem_fault.o minimem_shrinker.o minimem_scanner.o minimem_hook.o minimem_compat_check.o minimem_mmu.o

# Algorithm library sources (kernel-compatible)
minimem-y += lib/compressors/same_page.o
minimem-y += lib/compressors/bdi.o
minimem-y += lib/compressors/wkdm.o
minimem-y += lib/compressors/wkdm64.o
minimem-y += lib/compressors/block_class.o
minimem-y += lib/compressors/lz4_wrap.o
minimem-y += lib/compressors/delta.o
minimem-y += lib/advisor.o

# LZ4: lz4_wrap.c uses kernel's built-in LZ4 API when MINIMEM_KERNEL
# is defined. Vendored LZ4 source is NOT compiled for kernel.

ccflags-y := -I$(src)/include -I$(src) -I$(src)/lib -DMINIMEM_KERNEL

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install

.PHONY: all clean install