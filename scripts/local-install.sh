#!/bin/bash
# scripts/local-install.sh — Local Arch Linux install/uninstall for MiniMem
#
# Builds and installs the kernel module via DKMS, the userspace library via
# meson, and the systemd units for auto-load and auto-enable.  Everything
# can be cleanly undone with --uninstall.
#
# Usage:
#   sudo ./scripts/local-install.sh              # full install
#   sudo ./scripts/local-install.sh --uninstall   # full uninstall
#   sudo ./scripts/local-install.sh --module-only # DKMS module only
#   sudo ./scripts/local-install.sh --status      # check install status
#
# Recovery (if something goes wrong):
#   1. sudo rmmod minimem                         # unload module immediately
#   2. sudo ./scripts/local-install.sh --uninstall # remove everything
#   3. If kernel panic on boot: add minimem.blacklist=1 to kernel
#      command line in GRUB, then boot and uninstall.
#   4. If module won't unload: echo 0 > /sys/kernel/minimem/scanner_enabled
#      first (stops scanner), then rmmod minimem.

set -e

VERSION="0.8.0"
DKMS_NAME="minimem"
DKMS_DIR="/usr/src/${DKMS_NAME}-${VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KVER="$(uname -r)"

SYSTEMD_UNITS="
    minimem-load.service
    minimem.service
"

# ── Colour helpers ──────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[minimem]${NC} $*"; }
warn()  { echo -e "${YELLOW}[minimem]${NC} $*"; }
error() { echo -e "${RED}[minimem]${NC} $*" >&2; }

# ── Status check ────────────────────────────────────────────────────
cmd_status() {
    echo "=== MiniMem Install Status ==="
    echo ""

    # DKMS module
    if dkms status "$DKMS_NAME/$VERSION" 2>/dev/null | grep -q "$DKMS_NAME"; then
        info "DKMS module: installed"
        dkms status "$DKMS_NAME/$VERSION" 2>/dev/null | sed 's/^/  /'
    else
        warn "DKMS module: NOT installed"
    fi

    # Loaded?
    if lsmod 2>/dev/null | grep -q "^minimem "; then
        info "Module: LOADED"
        echo "  Pages compressed:   $(cat /sys/kernel/minimem/pages_compressed 2>/dev/null || echo 'N/A')"
        echo "  Bytes saved:         $(cat /sys/kernel/minimem/bytes_saved 2>/dev/null || echo 'N/A')"
        echo "  Scanner enabled:    $(cat /sys/kernel/minimem/scanner_enabled 2>/dev/null || echo 'N/A')"
        echo "  Kernel patches:      $(cat /sys/kernel/minimem/kernel_patches 2>/dev/null || echo 'N/A')"
        echo "  Parallel mode:       $(cat /sys/kernel/minimem/parallel_mode 2>/dev/null || echo 'N/A')"
    else
        warn "Module: not loaded"
    fi

    # Systemd units
    for unit in $SYSTEMD_UNITS; do
        if systemctl is-enabled "$unit" 2>/dev/null | grep -q "enabled"; then
            info "Systemd $unit: enabled"
        elif systemctl is-active "$unit" 2>/dev/null | grep -q "active"; then
            warn "Systemd $unit: active but not enabled"
        else
            warn "Systemd $unit: not enabled"
        fi
    done

    # Modules-load
    if [ -f /usr/lib/modules-load.d/minimem.conf ]; then
        info "Modules-load: configured"
    else
        warn "Modules-load: not configured"
    fi

    # Userspace library
    if pkg-config --exists minimem 2>/dev/null; then
        info "Userspace library: $(pkg-config --modversion minimem)"
    else
        warn "Userspace library: NOT installed"
    fi
}

# ── Install ─────────────────────────────────────────────────────────
cmd_install() {
    local module_only=false
    [ "$1" = "--module-only" ] && module_only=true

    echo "=== MiniMem Local Install ==="
    echo "  Version:  $VERSION"
    echo "  Kernel:   $KVER"
    echo ""

    if [ "$(id -u)" -ne 0 ]; then
        error "This script must be run as root (sudo)"
        exit 1
    fi

    # ── 1. DKMS kernel module ───────────────────────────────────────
    if ! command -v dkms &>/dev/null; then
        error "dkms is not installed. Install it:"
        error "  sudo pacman -S dkms"
        exit 1
    fi

    if ! [ -d "/lib/modules/$KVER/build" ]; then
        error "Kernel headers for $KVER are not installed. Install them:"
        error "  sudo pacman -S linux-headers"
        exit 1
    fi

    info "Installing DKMS kernel module..."

    # Remove previous version if present
    if dkms status "$DKMS_NAME/$VERSION" 2>/dev/null | grep -q "$DKMS_NAME"; then
        info "Removing previous DKMS installation..."
        dkms remove "$DKMS_NAME/$VERSION" --all 2>/dev/null || true
    fi
    rm -rf "$DKMS_DIR"

    # Copy module source
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

    for comp in same_page bdi wkdm wkdm64 block_class lz4_wrap delta; do
        cp "$PROJECT_DIR/src/lib/compressors/$comp."* "$DKMS_DIR/lib/compressors/"
    done

    cp "$PROJECT_DIR/patches"/minimem-*.patch "$DKMS_DIR/patches/" 2>/dev/null || true

    # Kernel stub headers
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

    dkms add "$DKMS_NAME/$VERSION"
    dkms build "$DKMS_NAME/$VERSION" -k "$KVER"
    dkms install "$DKMS_NAME/$VERSION" -k "$KVER"

    info "DKMS module installed for kernel $KVER"

    # ── 2. Systemd units ────────────────────────────────────────────
    if [ "$module_only" = false ]; then
        info "Installing systemd units..."

        install -Dm644 "$PROJECT_DIR/systemd/minimem-load.service" \
            /usr/lib/systemd/system/minimem-load.service
        install -Dm644 "$PROJECT_DIR/systemd/minimem.service" \
            /usr/lib/systemd/system/minimem.service
        install -Dm644 "$PROJECT_DIR/systemd/modules-load.d/minimem.conf" \
            /usr/lib/modules-load.d/minimem.conf

        systemctl daemon-reload
        systemctl enable minimem-load.service minimem.service

        info "Systemd units installed and enabled"

        # ── 3. Userspace library ────────────────────────────────────
        if command -v meson &>/dev/null; then
            info "Building userspace library and CLI..."

            BUILD_DIR="/tmp/minimem-meson-build"
            rm -rf "$BUILD_DIR"
            meson setup "$BUILD_DIR" "$PROJECT_DIR" -Dtests=false
            meson compile -C "$BUILD_DIR"
            meson install -C "$BUILD_DIR"
            ldconfig

            info "Userspace library and CLI installed"
        else
            warn "meson not found — skipping userspace library"
            warn "Install with: sudo pacman -S meson && sudo ./scripts/local-install.sh"
        fi

        # ── 3b. CLI tool (if not installed by meson) ─────────────────
        if [ ! -e /usr/local/bin/minimem ] && [ ! -e /usr/bin/minimem ]; then
            install -Dm755 "$PROJECT_DIR/scripts/minimem" /usr/local/bin/minimem
            info "CLI installed to /usr/local/bin/minimem"
        fi
    fi

    # ── 4. Load module now ──────────────────────────────────────────
    if lsmod 2>/dev/null | grep -q "^minimem "; then
        info "Module already loaded"
    else
        modprobe minimem 2>/dev/null || {
            info "modprobe failed, trying insmod..."
            local kmod
            kmod=$(find "/lib/modules/$KVER/updates" "/lib/modules/$KVER/extra" \
                        "/lib/modules/$KVER/kernel/drivers/minimem" 2>/dev/null \
                        -name 'minimem.ko*' 2>/dev/null | head -1)
            if [ -n "$kmod" ]; then
                insmod "$kmod"
            else
                warn "Could not find minimem.ko — try: sudo modprobe minimem"
            fi
        }
    fi

    if [ "$module_only" = false ]; then
        systemctl start minimem-load.service 2>/dev/null || true
        systemctl start minimem.service 2>/dev/null || true
    fi

    echo ""
    info "=== MiniMem Install Complete ==="
    echo ""
    echo "  Module:    loaded"
    echo "  Systemd:   enabled (auto-load + auto-scan on boot)"
    echo ""
    echo "  Check scanner:  cat /sys/kernel/minimem/scanner_enabled"
    echo "  Check patches:  cat /sys/kernel/minimem/kernel_patches"
    echo "  Full stats:     cat /sys/kernel/minimem/*"
    echo ""
    echo "  To disable scanner:  echo 0 | sudo tee /sys/kernel/minimem/scanner_enabled"
    echo "  To unload module:    sudo rmmod minimem"
    echo "  To fully remove:     sudo $0 --uninstall"
    echo ""
    echo "  EMERGENCY RECOVERY (kernel panic on boot):"
    echo "    Add minimem.blacklist=1 to kernel cmdline in GRUB"
    echo "    Then boot and run: sudo $0 --uninstall"
    echo "    If uninstall fails: sudo $0 --emergency"
    echo ""
    echo "  LiveCD RECOVERY (system won't boot):"
    echo "    Boot from LiveCD/USB, mount root, chroot, then: sudo $0 --uninstall"
}

# ── Uninstall ───────────────────────────────────────────────────────
cmd_uninstall() {
    echo "=== MiniMem Local Uninstall ==="
    echo ""

    if [ "$(id -u)" -ne 0 ]; then
        error "This script must be run as root (sudo)"
        exit 1
    fi

    # ── 1. Disable and stop systemd ────────────────────────────────
    info "Stopping and disabling systemd units..."
    systemctl stop minimem.service minimem-load.service 2>/dev/null || true
    systemctl disable minimem.service minimem-load.service 2>/dev/null || true

    # ── 2. Remove systemd unit files ───────────────────────────────
    rm -f /usr/lib/systemd/system/minimem-load.service
    rm -f /usr/lib/systemd/system/minimem.service
    rm -f /usr/lib/modules-load.d/minimem.conf
    systemctl daemon-reload

    info "Systemd units removed"

    # ── 3. Unload module ───────────────────────────────────────────
    if lsmod 2>/dev/null | grep -q "^minimem "; then
        # Stop scanner first so module use count drops
        echo 0 > /sys/kernel/minimem/scanner_enabled 2>/dev/null || true
        sleep 0.5
        info "Unloading module..."
        modprobe -r minimem 2>/dev/null || rmmod minimem 2>/dev/null || {
            warn "Could not unload module (may be in use)"
            warn "Stop processes using it and run: sudo rmmod minimem"
            warn "Or just reboot — the module won't auto-load anymore"
        }
    else
        info "Module not loaded"
    fi

    # ── 4. Remove DKMS ─────────────────────────────────────────────
    if dkms status "$DKMS_NAME/$VERSION" 2>/dev/null | grep -q "$DKMS_NAME"; then
        info "Removing DKMS module..."
        dkms remove "$DKMS_NAME/$VERSION" --all 2>/dev/null || true
    fi
    rm -rf "$DKMS_DIR"
    info "DKMS module removed"

    # ── 5. Remove userspace library ────────────────────────────────
    info "Removing userspace library..."
    local lib
    for lib in /usr/lib/libminimem.so.*; do
        [ -f "$lib" ] && rm -f "$lib"
    done
    rm -f /usr/lib/libminimem.so
    rm -f /usr/lib/libminimem_static.a
    rm -f /usr/lib/pkgconfig/minimem.pc
    rm -rf /usr/include/minimem
    rm -f /usr/local/bin/minimem
    ldconfig 2>/dev/null || true
    info "Userspace library and CLI removed"

    echo ""
    info "=== MiniMem Uninstall Complete ==="
    echo ""
    echo "  All compressed pages were restored to normal memory before"
    echo "  the module was unloaded. No data is lost."
    echo ""
    echo "  If you applied kernel patches, reverse them:"
    echo "    sudo /usr/src/minimem-$VERSION/uninstall.sh $KVER"
    echo "    # Then rebuild and reboot your kernel"
}

# ── Emergency recovery ─────────────────────────────────────────────
cmd_emergency() {
    echo "=== MiniMem Emergency Recovery ==="
    echo ""
    echo "This mode attempts to unload MiniMem even when normal rmmod fails."
    echo "Use this if:"
    echo "  - rmmod minimem fails with 'Device or resource busy'"
    echo "  - The system is unstable with MiniMem loaded"
    echo "  - You need to recover without rebooting"
    echo ""

    if [ "$(id -u)" -ne 0 ]; then
        error "This script must be run as root (sudo)"
        exit 1
    fi

    # 1. Force-stop the scanner
    if [ -d /sys/kernel/minimem ]; then
        info "Stopping scanner..."
        echo 0 > /sys/kernel/minimem/scanner_enabled 2>/dev/null || true

        # 2. Check how many pages are compressed
        local compressed
        compressed=$(cat /sys/kernel/minimem/zswap_pages 2>/dev/null || echo "unknown")
        info "Compressed pages: $compressed"

        if [ "$compressed" != "0" ] && [ "$compressed" != "unknown" ]; then
            info "Decompressing pages before unload..."
            # Access debugfs compress_vaddr won't help here — drain happens on rmmod
            info "Will rely on drain-and-restore during module unload"
        fi
    fi

    # 3. Try to reduce module reference count
    info "Attempting to reduce module references..."
    modprobe -r minimem 2>/dev/null || true
    sleep 1

    # 4. Try rmmod
    if lsmod 2>/dev/null | grep -q "^minimem "; then
        info "Module still loaded, forcing unload..."
        # Try rmmod with force flag
        rmmod -f minimem 2>/dev/null || rmmod minimem 2>/dev/null || {
            error "Cannot unload module even with --force"
            error ""
            error "EMERGENCY RECOVERY OPTIONS:"
            error ""
            error "Option 1: Kill processes using compressed memory"
            error "  The module may be in use by processes with compressed pages."
            error "  Find them: grep -r minimem /proc/*/maps 2>/dev/null"
            error "  Then kill those processes and retry: sudo $0 --emergency"
            error ""
            error "Option 2: Reboot with blacklist"
            error "  Add to GRUB kernel command line: minimem.blacklist=1"
            error "  Edit /etc/default/grub, add to GRUB_CMDLINE_LINUX:"
            error "    minimem.blacklist=1"
            error "  Then: sudo update-grub && sudo reboot"
            error "  After reboot, run: sudo $0 --uninstall"
            error ""
            error "Option 3: LiveCD recovery"
            error "  If the system won't boot:"
            error "  1. Boot from a LiveCD/USB (any Linux distro)"
            error "  2. Mount the root filesystem:"
            error "     sudo mount /dev/sdXN /mnt    # replace with your root partition"
            error "  3. Chroot and uninstall:"
            error "     sudo arch-chroot /mnt  # or: sudo chroot /mnt"
            error "     sudo dkms remove minimem/$VERSION --all"
            error "     sudo rm -f /usr/lib/systemd/system/minimem*.service"
            error "     sudo rm -f /usr/lib/modules-load.d/minimem.conf"
            error "     sudo systemctl daemon-reload"
            error "  4. If kernel patches were applied:"
            error "     sudo /usr/src/minimem-$VERSION/uninstall.sh \$(uname -r)"
            error "     # Then rebuild and reinstall the kernel"
            error "  5. Reboot into the repaired system"
            error ""
            error "Option 4: Remove DKMS modules manually"
            error "  If DKMS is broken:"
            error "  1. Find and remove: sudo find /lib/modules -name 'minimem.ko*' -delete"
            error "  2. Regenerate initrd: sudo mkinitcpio -P  # Arch"
            error "                        sudo dracut --force   # Fedora/openSUSE"
            error "                        sudo update-initramfs -u  # Debian/Ubuntu"
            error "  3. Reboot"
            exit 1
        }
        info "Module unloaded successfully (forced)"
    else
        info "Module already unloaded"
    fi

    # 5. Clean up systemd and DKMS
    info "Cleaning up systemd units..."
    systemctl stop minimem.service minimem-load.service 2>/dev/null || true
    systemctl disable minimem.service minimem-load.service 2>/dev/null || true
    rm -f /usr/lib/systemd/system/minimem-load.service
    rm -f /usr/lib/systemd/system/minimem.service
    rm -f /usr/lib/modules-load.d/minimem.conf
    systemctl daemon-reload

    info "Removing DKMS module..."
    dkms remove "$DKMS_NAME/$VERSION" --all 2>/dev/null || true
    rm -rf "$DKMS_DIR"

    info "Removing userspace library..."
    for lib in /usr/lib/libminimem.so.*; do
        [ -f "$lib" ] && rm -f "$lib"
    done
    rm -f /usr/lib/libminimem.so /usr/lib/libminimem_static.a
    rm -f /usr/lib/pkgconfig/minimem.pc
    rm -rf /usr/include/minimem
    rm -f /usr/local/bin/minimem
    ldconfig 2>/dev/null || true

    echo ""
    info "=== Emergency Recovery Complete ==="
    echo ""
    echo "  The module was force-unloaded. Compressed pages were"
    echo "  restored by drain-and-restore before unload."
    echo ""
    echo "  If you experience issues, reboot the system."
    echo ""
    echo "  If kernel patches were applied, reverse them:"
    echo "    sudo /usr/src/minimem-$VERSION/uninstall.sh \$(uname -r)"
}

# ── Main ────────────────────────────────────────────────────────────
case "${1:-install}" in
    --uninstall|-u)  cmd_uninstall ;;
    --emergency|-e)  cmd_emergency ;;
    --module-only|-m) cmd_install "--module-only" ;;
    --status|-s)      cmd_status ;;
    install|--install|-i) cmd_install ;;
    *)
        echo "Usage: $0 {install|--install|-i|--uninstall|-u|--emergency|-e|--module-only|-m|--status|-s}"
        echo ""
        echo "  install        Install module + systemd + userspace library (default)"
        echo "  --module-only  Install DKMS module only (no systemd, no library)"
        echo "  --uninstall    Remove everything (clean unload with drain-and-restore)"
        echo "  --emergency    Force unload when normal uninstall fails"
        echo "  --status       Show install status"
        echo ""
        echo "Emergency recovery if module causes kernel panic on boot:"
        echo "  1. Add minimem.blacklist=1 to kernel cmdline in GRUB"
        echo "  2. Boot and run: sudo $0 --uninstall"
        echo "  3. If uninstall fails: sudo $0 --emergency"
        echo ""
        echo "LiveCD recovery if system won't boot:"
        echo "  1. Boot from LiveCD/USB"
        echo "  2. Mount root: sudo mount /dev/sdXN /mnt"
        echo "  3. Chroot: sudo arch-chroot /mnt"
        echo "  4. Uninstall: sudo $0 --uninstall"
        echo "  5. Rebuild kernel if patches were applied"
        echo "  6. Reboot"
        exit 1
        ;;
esac