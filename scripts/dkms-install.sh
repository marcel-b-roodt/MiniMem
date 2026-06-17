#!/bin/bash
# scripts/dkms-install.sh — Install MiniMem as a DKMS module
#
# This copies the module source into /usr/src/minimem-0.6.0/,
# registers it with DKMS, builds it for the current kernel,
# and optionally applies kernel patches for full functionality.
#
# Usage: sudo ./scripts/dkms-install.sh [--patches]
#
#   --patches   Also apply kernel patches and show rebuild instructions

set -e

VERSION="0.7.0"
DKMS_NAME="minimem"
DKMS_DIR="/usr/src/${DKMS_NAME}-${VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KVER="$(uname -r)"
APPLY_PATCHES=false

for arg in "$@"; do
    case "$arg" in
        --patches) APPLY_PATCHES=true ;;
    esac
done

echo "=== MiniMem DKMS Install ==="
echo "  Version: $VERSION"
echo "  Kernel:  $KVER"
echo "  Target:  $DKMS_DIR"
echo ""

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root (sudo)"
    exit 1
fi

if ! command -v dkms &>/dev/null; then
    echo "Error: dkms is not installed."
    echo "Install it:  sudo pacman -S dkms  (Arch)"
    echo "            sudo apt install dkms  (Debian/Ubuntu)"
    echo "            sudo dnf install dkms  (Fedora)"
    exit 1
fi

if [ -d "$DKMS_DIR" ]; then
    echo "Removing previous DKMS installation..."
    dkms remove "$DKMS_NAME/$VERSION" --all 2>/dev/null || true
    rm -rf "$DKMS_DIR"
fi

echo "Copying module source to $DKMS_DIR ..."
mkdir -p "$DKMS_DIR"
mkdir -p "$DKMS_DIR/lib/compressors"
mkdir -p "$DKMS_DIR/include"
mkdir -p "$DKMS_DIR/patches"

cp "$PROJECT_DIR/dkms/dkms.conf" "$DKMS_DIR/"
cp "$PROJECT_DIR/dkms/Makefile" "$DKMS_DIR/"
cp "$PROJECT_DIR/dkms/install.sh" "$DKMS_DIR/"
cp "$PROJECT_DIR/dkms/uninstall.sh" "$DKMS_DIR/"

cp "$PROJECT_DIR/src/kernel"/*.c "$DKMS_DIR/"
cp "$PROJECT_DIR/src/kernel"/*.h "$DKMS_DIR/"

cp "$PROJECT_DIR/src/lib/minimem.h" "$DKMS_DIR/lib/"
cp "$PROJECT_DIR/src/lib/advisor.c" "$DKMS_DIR/lib/"
cp "$PROJECT_DIR/src/lib/advisor.h" "$DKMS_DIR/lib/"

cp "$PROJECT_DIR/src/lib/compressors/same_page."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/bdi."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/wkdm."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/wkdm64."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/block_class."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/lz4_wrap."* "$DKMS_DIR/lib/compressors/"
cp "$PROJECT_DIR/src/lib/compressors/delta."* "$DKMS_DIR/lib/compressors/"

cp "$PROJECT_DIR/patches/minimem-"*.patch "$DKMS_DIR/patches/"

cat > "$DKMS_DIR/include/string.h" << 'STUB'
#ifndef _STRING_H
#define _STRING_H
#include <linux/string.h>
#endif
STUB

cat > "$DKMS_DIR/include/stdbool.h" << 'STUB'
#ifndef __STDBOOL_H
#define __STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
STUB

cat > "$DKMS_DIR/include/stdint.h" << 'STUB'
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

cat > "$DKMS_DIR/include/stddef.h" << 'STUB'
#ifndef _STDDEF_H
#define _STDDEF_H
#include <linux/stddef.h>
#endif
STUB

cat > "$DKMS_DIR/include/stdlib.h" << 'STUB'
#ifndef _STDLIB_H
#define _STDLIB_H
#include <linux/slab.h>
#include <linux/kernel.h>
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define calloc(n, s) kcalloc(n, s, GFP_KERNEL)
#define free(x) kfree(x)
#endif
STUB

cat > "$DKMS_DIR/include/limits.h" << 'STUB'
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

echo "Registering with DKMS ..."
dkms add "$DKMS_NAME/$VERSION"

echo "Building for kernel $KVER ..."
dkms build "$DKMS_NAME/$VERSION" -k "$KVER"

echo "Installing for kernel $KVER ..."
dkms install "$DKMS_NAME/$VERSION" -k "$KVER"

if [ "$APPLY_PATCHES" = true ]; then
    echo ""
    echo "Applying kernel patches ..."
    bash "$DKMS_DIR/install.sh" "$KVER"
fi

echo ""
echo "=== MiniMem DKMS Install Complete ==="
echo ""
echo "Module status:"
dkms status "$DKMS_NAME/$VERSION"
echo ""
echo "To load the module (no reboot needed):"
echo "  sudo modprobe minimem"
echo ""
echo "To check if kernel patches are detected:"
echo "  cat /sys/kernel/minimem/kernel_patches"
echo "    0 = kprobe-only mode (scanner sweep disabled)"
echo "    1 = patched kernel (scanner sweep enabled)"
echo ""
echo "To enable full scanner sweep (requires reboot):"
echo "  sudo $DKMS_DIR/install.sh $KVER"
echo "  # Then rebuild and reboot the kernel"
echo ""
echo "DKMS will auto-rebuild the module when your kernel updates."
echo "You will need to re-apply kernel patches after each kernel update."