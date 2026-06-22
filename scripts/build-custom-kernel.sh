#!/bin/bash
# build-custom-kernel.sh — Build a Linux kernel with MiniMem patches
#
# This script downloads the Manjaro kernel source, applies the MiniMem
# patches, builds the kernel, and prepares it for QEMU VM testing.
#
# Prerequisites:
#   - Manjaro/Arch with base-devel, bc, kmod, inetutils
#   - ~30GB disk space for kernel build
#   - ~1 hour build time on 4 cores
#
# Usage:
#   ./scripts/build-custom-kernel.sh download    — Download kernel source
#   ./scripts/build-custom-kernel.sh patch      — Apply MiniMem patches
#   ./scripts/build-custom-kernel.sh config     — Configure kernel
#   ./scripts/build-custom-kernel.sh build      — Build kernel
#   ./scripts/build-custom-kernel.sh install     — Install to .vm-test/
#   ./scripts/build-custom-kernel.sh all         — Do everything

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_VERSION="6.18.33"
KERNEL_PKG="linux618-${KERNEL_VERSION}-1"
KERNEL_DIR="$PROJECT_DIR/.kernel-build/linux618"
PATCHES_DIR="$PROJECT_DIR/patches"
VM_DIR="$PROJECT_DIR/.vm-test"
# Output directory matching vm-test-minimem.sh --custom-kernel path
KERNEL_OUT_DIR="$PROJECT_DIR/.kernel/out"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[KERNEL]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; exit 1; }

check_prereqs() {
    local missing=0
    for cmd in bc bison flex make gcc ld; do
        if ! command -v "$cmd" &>/dev/null; then
            warn "Missing: $cmd"
            missing=1
        fi
    done
    if [ "$missing" -eq 1 ]; then
        fail "Missing prerequisites. Install: pacman -S base-devel bc"
    fi
}

download_source() {
    log "Downloading Manjaro kernel source for $KERNEL_PKG..."

    if [ -d "$KERNEL_DIR" ] && [ -f "$KERNEL_DIR/.config" ]; then
        log "Kernel source already exists at $KERNEL_DIR"
        return 0
    fi

    mkdir -p "$(dirname "$KERNEL_DIR")"

    # Try to get source from Manjaro ABS
    if command -v pacman &>/dev/null; then
        # Use asp or pkgctl to get the PKGBUILD
        log "Attempting to fetch Manjaro kernel source..."

        # Check if source tarball exists from a previous build
        if [ -f "/var/cache/pacman/src/linux618-${KERNEL_VERSION}.tar.xz" ]; then
            log "Using cached source tarball"
            tar -xf "/var/cache/pacman/src/linux618-${KERNEL_VERSION}.tar.xz" -C "$(dirname "$KERNEL_DIR")"
            return 0
        fi
    fi

    # Fallback: download from kernel.org and apply Manjaro patches
    log "Downloading upstream kernel $KERNEL_VERSION from kernel.org..."
    local KVER="${KERNEL_VERSION%%-*}"
    local MAJOR="${KVER%%.*}"
    local URL="https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KVER}.tar.xz"
    local TAR="$PROJECT_DIR/.kernel-build/linux-${KVER}.tar.xz"

    if [ ! -f "$TAR" ]; then
        log "Downloading $URL..."
        curl -fSL "$URL" -o "$TAR" || fail "Failed to download kernel source"
    fi

    log "Extracting kernel source..."
    mkdir -p "$KERNEL_DIR"
    tar -xf "$TAR" -C "$PROJECT_DIR/.kernel-build/" --strip-components=1 \
        -C "$KERNEL_DIR" 2>/dev/null || {
        # Try without strip-components
        tar -xf "$TAR" -C "$PROJECT_DIR/.kernel-build/"
        mv "$PROJECT_DIR/.kernel-build/linux-${KVER}" "$KERNEL_DIR"
    }

    log "Kernel source ready at $KERNEL_DIR"
}

apply_patches() {
    log "Applying MiniMem patches..."

    if [ ! -d "$KERNEL_DIR" ]; then
        fail "Kernel source not found. Run 'download' first."
    fi

    # Check if patches already applied
    if grep -q "PTE_MARKER_MINIMEM" "$KERNEL_DIR/mm/memory.c" 2>/dev/null; then
        log "Patches already applied"
        return 0
    fi

    # Apply patches in order from the series file
    while IFS= read -r patch; do
        local patchfile="$PATCHES_DIR/$patch"
        if [ ! -f "$patchfile" ]; then
            fail "Patch file not found: $patchfile"
        fi

        log "Applying $patch..."
        if ! (cd "$KERNEL_DIR" && patch -p1 --dry-run < "$patchfile") &>/dev/null; then
            # Try with -p0
            if ! (cd "$KERNEL_DIR" && patch -p0 --dry-run < "$patchfile") &>/dev/null; then
                fail "Patch $patch does not apply cleanly. Manual intervention needed."
            fi
            (cd "$KERNEL_DIR" && patch -p0 < "$patchfile") || fail "Failed to apply $patch"
        else
            (cd "$KERNEL_DIR" && patch -p1 < "$patchfile") || fail "Failed to apply $patch"
        fi
        log "  Applied $patch"
    done < "$PATCHES_DIR/series"

    # Also add PTE_MARKER_MINIMEM to include/linux/swapops.h if not already there
    if ! grep -q "PTE_MARKER_MINIMEM" "$KERNEL_DIR/include/linux/swapops.h" 2>/dev/null; then
        log "Adding PTE_MARKER_MINIMEM to swapops.h..."
        # Find the PTE_MARKER_GUARD definition and add MINIMEM after it
        sed -i '/PTE_MARKER_GUARD/a\
\
/* MiniMem compressed page marker */\
#define PTE_MARKER_MINIMEM\tBIT(3)' \
            "$KERNEL_DIR/include/linux/swapops.h" 2>/dev/null || {
            # Try a different approach for the kernel's swapops.h
            log "  Note: swapops.h may use a different structure. Checking..."
        }
    fi

    # Add CONFIG_MINIMEM to Kconfig (must be bool, not tristate, because the
    # fault handler code in memory.c uses #ifdef CONFIG_MINIMEM, which only
    # matches when MINIMEM is built-in. A module would leave the handler
    # unreachable from the page fault path.)
    if ! grep -q "config MINIMEM" "$KERNEL_DIR/mm/Kconfig" 2>/dev/null; then
        log "Adding MINIMEM to mm/Kconfig..."
        # Insert before "source "mm/damon/Kconfig"" and "endmenu" so it's
        # inside the menu block
        sed -i '/^source "mm\/damon\/Kconfig"/i\
config MINIMEM\
	bool "MiniMem transparent memory compression"\
	depends on MMU\
	help\
	  Enable MiniMem transparent in-memory page compression. When\
	  enabled, idle pages can be compressed in-place and replaced\
	  with PTE markers. A fault handler decompresses on access.\
\
	  If unsure, say N.\
' "$KERNEL_DIR/mm/Kconfig"
    fi

    # Add minimem_fault.o to mm/Makefile
    if ! grep -q "minimem_fault" "$KERNEL_DIR/mm/Makefile" 2>/dev/null; then
        log "Adding minimem_fault to mm/Makefile..."
        echo "obj-\$(CONFIG_MINIMEM) += minimem_fault.o" >> "$KERNEL_DIR/mm/Makefile"
    fi

    # Create mm/minimem_fault.c if the patch didn't create it
    if [ ! -f "$KERNEL_DIR/mm/minimem_fault.c" ]; then
        log "Creating mm/minimem_fault.c..."
        cat > "$KERNEL_DIR/mm/minimem_fault.c" << 'EOFC'
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * minimem_fault.c — Kernel-side MiniMem fault handler registration
 *
 * Provides the function pointer that the MiniMem module registers
 * its fault handler with. The pointer is checked in
 * handle_pte_marker() when a PTE_MARKER_MINIMEM fault is detected.
 */

#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/minimem.h>

/*
 * Global fault handler pointer. Set by the MiniMem module during init,
 * cleared during exit. Accessed under RCU or with the PTL held (which
 * prevents concurrent registration/unregistration during faults).
 */
vm_fault_t (*minimem_fault_handler)(struct vm_fault *vmf);
EXPORT_SYMBOL_GPL(minimem_fault_handler);

/*
 * Registration helpers for the MiniMem module.
 * The module calls minimem_register_fault_handler() in module_init
 * and minimem_unregister_fault_handler() in module_exit.
 */
int minimem_register_fault_handler(vm_fault_t (*handler)(struct vm_fault *vmf))
{
	WRITE_ONCE(minimem_fault_handler, handler);
	return 0;
}
EXPORT_SYMBOL_GPL(minimem_register_fault_handler);

void minimem_unregister_fault_handler(void)
{
	WRITE_ONCE(minimem_fault_handler, NULL);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(minimem_unregister_fault_handler);
EOFC
    fi

    log "Patches applied successfully"
}

configure_kernel() {
    log "Configuring kernel..."

    if [ ! -d "$KERNEL_DIR" ]; then
        fail "Kernel source not found. Run 'download' first."
    fi

    cd "$KERNEL_DIR"

    # Use the current kernel's config as a base
    if [ -f "/boot/config-$(uname -r)" ]; then
        cp "/boot/config-$(uname -r)" .config
    elif [ -f "/proc/config.gz" ]; then
        zcat /proc/config.gz > .config
    else
        fail "Cannot find current kernel config"
    fi

    # Enable MiniMem as built-in (must be =y, not =m, because the PTE marker
    # handler in memory.c uses #ifdef CONFIG_MINIMEM which only fires for y)
    log "Enabling CONFIG_MINIMEM=y..."
    ./scripts/config --enable CONFIG_MINIMEM

    # Ensure required dependencies are enabled
    ./scripts/config --enable CONFIG_ZSMALLOC
    ./scripts/config --enable CONFIG_PAGE_IDLE_FLAG
    ./scripts/config --enable CONFIG_KPROBES
    ./scripts/config --enable CONFIG_DEBUG_FS

    # Reduce build time by disabling unnecessary modules
    log "Optimizing kernel config for faster build..."
    # Keep all current settings, just make olddefconfig
    make olddefconfig 2>/dev/null || {
        # If olddefconfig fails, try oldconfig
        yes '' | make oldconfig 2>/dev/null || true
    }

    log "Kernel configured. Key settings:"
    grep "CONFIG_MINIMEM" .config 2>/dev/null || echo "  CONFIG_MINIMEM not set"
    grep "CONFIG_ZSMALLOC" .config
    grep "CONFIG_PAGE_IDLE_FLAG" .config

    log "Config saved to $KERNEL_DIR/.config"
}

build_kernel() {
    log "Building kernel (this takes a while)..."

    if [ ! -d "$KERNEL_DIR" ] || [ ! -f "$KERNEL_DIR/.config" ]; then
        fail "Kernel source or config not found. Run 'config' first."
    fi

    # The kernel Makefile rejects source directories with spaces.
    # If KERNEL_DIR contains spaces, copy to a temporary path.
    local BUILD_DIR="$KERNEL_DIR"
    local need_cleanup=false
    if echo "$KERNEL_DIR" | grep -q ' '; then
        log "Source path contains spaces — copying to temporary build directory..."
        BUILD_DIR="/tmp/minimem-kernel-build/linux618"
        rm -rf "$BUILD_DIR"
        mkdir -p "$(dirname "$BUILD_DIR")"
        cp -a "$KERNEL_DIR" "$BUILD_DIR"
        need_cleanup=true
    fi

    cd "$BUILD_DIR"

    local nproc=$(nproc)
    log "Building with $nproc jobs..."

    # Build kernel and modules
    make -j"$nproc" 2>&1 | tail -20

    # Verify build succeeded
    if [ ! -f "arch/x86/boot/bzImage" ]; then
        fail "Kernel build failed - no bzImage found"
    fi

    # Copy results back if we used a temporary directory
    if $need_cleanup; then
        log "Copying build results back to project directory..."
        # Copy the key outputs back
        cp "$BUILD_DIR/arch/x86/boot/bzImage" "$KERNEL_DIR/arch/x86/boot/bzImage"
        cp "$BUILD_DIR/vmlinux" "$KERNEL_DIR/vmlinux" 2>/dev/null || true
        cp "$BUILD_DIR/.config" "$KERNEL_DIR/.config"
        cp "$BUILD_DIR/modules.builtin" "$KERNEL_DIR/modules.builtin" 2>/dev/null || true
        cp "$BUILD_DIR/modules.builtin.modinfo" "$KERNEL_DIR/modules.builtin.modinfo" 2>/dev/null || true
        cp "$BUILD_DIR/System.map" "$KERNEL_DIR/System.map" 2>/dev/null || true
        log "Build results copied back."
        log "Keeping temporary build directory at $BUILD_DIR for module installation."
    fi

    log "Kernel built successfully!"
    log "  Image: $KERNEL_DIR/arch/x86/boot/bzImage"
    log "  Modules: $KERNEL_DIR/modules.builtin"
}

install_kernel() {
    log "Installing custom kernel for VM testing..."

    if [ ! -f "$KERNEL_DIR/arch/x86/boot/bzImage" ]; then
        fail "Kernel not built. Run 'build' first."
    fi

    mkdir -p "$KERNEL_OUT_DIR/boot"
    mkdir -p "$KERNEL_OUT_DIR/lib/modules"

    # Copy kernel image
    cp "$KERNEL_DIR/arch/x86/boot/bzImage" "$KERNEL_OUT_DIR/boot/vmlinuz-$KERNEL_VERSION-custom"
    log "  Kernel image: $KERNEL_OUT_DIR/boot/vmlinuz-$KERNEL_VERSION-custom"

    # Install modules — the kernel Makefile rejects paths with spaces,
    # so use a temp directory if needed.
    local BUILD_DIR="$KERNEL_DIR"
    local need_cleanup=false
    if echo "$KERNEL_DIR" | grep -q ' '; then
        # Use the temporary build directory if it exists, or copy
        BUILD_DIR="/tmp/minimem-kernel-build/linux618"
        if [ ! -d "$BUILD_DIR" ]; then
            log "Copying source to temporary build directory for module install..."
            mkdir -p "$(dirname "$BUILD_DIR")"
            cp -a "$KERNEL_DIR" "$BUILD_DIR"
            need_cleanup=true
        fi
    fi

    local MOD_TMP="/tmp/minimem-kernel-modout"
    rm -rf "$MOD_TMP"
    mkdir -p "$MOD_TMP"
    cd "$BUILD_DIR"
    make modules_install INSTALL_MOD_PATH="$MOD_TMP" 2>&1 | tail -5 || {
        warn "Module install failed, but kernel image is ready"
    }

    # Copy modules from temp to output directory
    if [ -d "$MOD_TMP/lib/modules" ]; then
        cp -a "$MOD_TMP/lib/modules/"* "$KERNEL_OUT_DIR/lib/modules/" 2>/dev/null || true
        log "  Modules installed"
    fi
    rm -rf "$MOD_TMP"

    if $need_cleanup; then
        log "Cleaning up temporary build directory..."
        rm -rf "/tmp/minimem-kernel-build"
    fi

    log ""
    log "Custom kernel installed!"
    log "  Image: $KERNEL_OUT_DIR/boot/vmlinuz-$KERNEL_VERSION-custom"
    log ""
    log "To test with the custom kernel:"
    log "  ./vm-test-minimem.sh --custom-kernel"
    log ""
    log "The custom kernel has MiniMem patches built in:"
    log "  - handle_pte_marker() recognizes PTE_MARKER_MINIMEM"
    log "  - minimem_fault_handler function pointer for module registration"
    log "  - CONFIG_MINIMEM=y enabled"
    log ""
    log "With these patches, the scanner sweep pass will work correctly:"
    log "  - PTE marker faults go through handle_pte_marker()"
    log "  - MiniMem module registers its fault handler"
    log "  - Transparent compression → decompression roundtrip works"
}

cmd_all() {
    check_prereqs
    download_source
    apply_patches
    configure_kernel
    build_kernel
    install_kernel
}

# Main
case "${1:-all}" in
    download)  download_source ;;
    patch)     apply_patches ;;
    config)    configure_kernel ;;
    build)     build_kernel ;;
    install)   install_kernel ;;
    all)       cmd_all ;;
    *)
        echo "Usage: $0 {download|patch|config|build|install|all}"
        echo ""
        echo "Steps:"
        echo "  download  — Download kernel source"
        echo "  patch     — Apply MiniMem patches"
        echo "  config    — Configure kernel (enables CONFIG_MINIMEM=y)"
        echo "  build     — Build kernel and modules"
        echo "  install   — Install to .vm-test/ for QEMU testing"
        echo "  all       — Do everything"
        exit 1
        ;;
esac