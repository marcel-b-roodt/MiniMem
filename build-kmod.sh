#!/bin/bash
# Build MiniMem kernel module from a path without spaces
# (The kernel build system doesn't handle spaces in M= paths)
#
# Usage: ./build-kmod.sh           # build
#        ./build-kmod.sh clean     # clean
#        ./build-kmod.sh load      # load module
#        ./build-kmod.sh unload    # unload module
#        ./build-kmod.sh stats     # show sysfs stats

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KVER="$(uname -r)"
KBUILD="/tmp/minimem-kbuild"

case "${1:-build}" in
    clean)
        rm -rf "$KBUILD"
        echo "Cleaned build directory"
        ;;
    build|"")
        rm -rf "$KBUILD"
        mkdir -p "$KBUILD/src/kernel/lib/compressors"
        mkdir -p "$KBUILD/src/kernel/lib/vendor/lz4"
        mkdir -p "$KBUILD/src/kernel/include"

        # Copy kernel module core
        cp "$SCRIPT_DIR"/src/kernel/*.c "$SCRIPT_DIR"/src/kernel/*.h "$KBUILD/src/kernel/"

        # Copy algorithm library sources
        cp "$SCRIPT_DIR"/src/lib/minimem.h "$KBUILD/src/kernel/lib/"
        cp "$SCRIPT_DIR"/src/lib/advisor.c "$SCRIPT_DIR"/src/lib/advisor.h "$KBUILD/src/kernel/lib/"
        cp "$SCRIPT_DIR"/src/lib/compressors/same_page.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/bdi.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/wkdm.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/wkdm64.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/block_class.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/lz4_wrap.* "$KBUILD/src/kernel/lib/compressors/"
        cp "$SCRIPT_DIR"/src/lib/compressors/delta.* "$KBUILD/src/kernel/lib/compressors/"

        # Write Makefile
        cat > "$KBUILD/src/kernel/Makefile" << 'MAKEEOF'
obj-m += minimem.o
minimem-y := minimem_main.o minimem_compress.o minimem_map.o minimem_zswap.o minimem_parallel.o minimem_fault.o minimem_shrinker.o minimem_scanner.o minimem_hook.o minimem_proc_stats.o
minimem-y += lib/compressors/same_page.o
minimem-y += lib/compressors/bdi.o
minimem-y += lib/compressors/wkdm.o
minimem-y += lib/compressors/wkdm64.o
minimem-y += lib/compressors/block_class.o
minimem-y += lib/compressors/lz4_wrap.o
minimem-y += lib/compressors/delta.o
minimem-y += lib/advisor.o
ccflags-y := -I$(src)/include -I$(src) -I$(src)/lib -DMINIMEM_KERNEL
all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
.PHONY: all clean
MAKEEOF

        # Create libc stub headers for kernel build
        cat > "$KBUILD/src/kernel/include/string.h" << 'STUB'
#ifndef _STRING_H
#define _STRING_H
#include <linux/string.h>
#endif
STUB
        cat > "$KBUILD/src/kernel/include/stdbool.h" << 'STUB'
#ifndef __STDBOOL_H
#define __STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
STUB
        cat > "$KBUILD/src/kernel/include/stdint.h" << 'STUB'
#ifndef _STDINT_H
#define _STDINT_H
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef s8 int8_t;
typedef s16 int16_t;
typedef s32 int32_t;
typedef s64 int64_t;
#endif
STUB
        cat > "$KBUILD/src/kernel/include/stddef.h" << 'STUB'
#ifndef _STDDEF_H
#define _STDDEF_H
#include <linux/stddef.h>
#endif
STUB
        cat > "$KBUILD/src/kernel/include/stdlib.h" << 'STUB'
#ifndef _STDLIB_H
#define _STDLIB_H
#include <linux/slab.h>
#include <linux/kernel.h>
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define calloc(n, s) kcalloc(n, s, GFP_KERNEL)
#define free(x) kfree(x)
#endif
STUB
        cat > "$KBUILD/src/kernel/include/limits.h" << 'STUB'
#ifndef _LIMITS_H
#define _LIMITS_H
#include <linux/limits.h>
#include <linux/kernel.h>
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#ifndef SIZE_MAX
#define SIZE_MAX (~(size_t)0)
#endif
#endif
STUB

        echo "Building MiniMem kernel module..."
        echo "  Kernel: $KVER"
        make -C /lib/modules/$KVER/build M="$KBUILD/src/kernel" modules 2>&1

        # Copy module back to source tree
        cp "$KBUILD/src/kernel/minimem.ko" "$SCRIPT_DIR/src/kernel/minimem.ko"
        echo ""
        echo "Module built: src/kernel/minimem.ko"
        echo "Algorithms compiled: same_page, BDI, WKdm, WKdm-64, block_class, LZ4, delta"
        echo ""
        echo "To load:   sudo insmod src/kernel/minimem.ko"
        echo "To unload: sudo rmmod minimem"
        echo "Stats:     cat /sys/kernel/minimem/*"
        ;;
    tests)
        echo "Building VM test binaries..."
        mkdir -p "$SCRIPT_DIR/tests/kernel"

        for src in test_stress_concurrent test_stress_pressure test_stress_unload test_transparent_e2e; do
            if [ -f "$SCRIPT_DIR/tests/kernel/${src}.c" ]; then
                echo "  Building $src ..."
                gcc -static -o "$SCRIPT_DIR/tests/kernel/$src" \
                    "$SCRIPT_DIR/tests/kernel/${src}.c" \
                    -lpthread 2>/dev/null || \
                gcc -static -o "$SCRIPT_DIR/tests/kernel/$src" \
                    "$SCRIPT_DIR/tests/kernel/${src}.c" 2>/dev/null || {
                    echo "  Warning: failed to build $src"
                    continue
                }
            fi
        done

        echo "Test binaries built in tests/kernel/"
        ;;
    load)
        sudo insmod "$SCRIPT_DIR/src/kernel/minimem.ko"
        echo "Module loaded. Stats:"
        for f in /sys/kernel/minimem/*; do
            echo "  $(basename $f): $(cat $f 2>/dev/null)"
        done
        ;;
    unload)
        sudo rmmod minimem
        echo "Module unloaded"
        ;;
    stats)
        for f in /sys/kernel/minimem/*; do
            echo "$(basename $f): $(cat $f 2>/dev/null)"
        done
        ;;
    *)
        echo "Usage: $0 {build|clean|tests|load|unload|stats}"
        echo ""
        echo "  build   - Build the kernel module (default)"
        echo "  clean   - Remove build directory"
        echo "  tests   - Build static VM test binaries"
        echo "  load    - Load the module with sudo"
        echo "  unload  - Unload the module with sudo"
        echo "  stats   - Show /sys/kernel/minimem/ stats"
        exit 1
        ;;
esac