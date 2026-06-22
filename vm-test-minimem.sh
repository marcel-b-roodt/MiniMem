#!/bin/bash
# vm-test-minimem.sh — QEMU-based MiniMem kernel module test
#
# Creates a minimal VM, boots it with the host kernel, loads the MiniMem
# module, runs all three benchmark modes (baseline/serial/parallel), and
# reports results. Completely isolated from the host — kernel panics in
# the guest cannot affect the host.
#
# Prerequisites: qemu-base (qemu-system-x86_64)
#
# Usage:
#   ./vm-test-minimem.sh              # Full test (build rootfs, boot, test)
#   ./vm-test-minimem.sh --8gb        # 8GB RAM performance test (4 CPUs, 300s timeout)
#   ./vm-test-minimem.sh --4gb        # 4GB RAM test
#   ./vm-test-minimem.sh --2gb        # 2GB RAM test
#   ./vm-test-minimem.sh --ram 8G     # Custom RAM size
#   ./vm-test-minimem.sh --rebuild    # Force rebuild rootfs
#   ./vm-test-minimem.sh --shell      # Boot VM and drop to shell
#   ./vm-test-minimem.sh --skip-parallel  # Skip parallel benchmark
#   ./vm-test-minimem.sh --custom-kernel  # Use custom kernel from .kernel/out/

set -euo pipefail

# Kill any lingering QEMU processes on exit or interrupt
cleanup_qemu() {
    # Kill QEMU processes that we spawned
    pkill -f "qemu-system-x86_64.*minimem" 2>/dev/null || true
}
trap cleanup_qemu EXIT INT TERM

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE="$SCRIPT_DIR/src/kernel/minimem.ko"
VM_DIR="$SCRIPT_DIR/.vm-test"
ROOTFS="$VM_DIR/rootfs.cpio.gz"
# KERNEL and INITRD are resolved in check_prereqs() based on --custom-kernel flag

# VM settings
VM_RAM="768M"
VM_CPUS="4"
VM_TIMEOUT="180"

SKIP_PARALLEL=false
SHELL_MODE=false
FORCE_REBUILD=false
CUSTOM_KERNEL=false
VM_RAM_SET=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()   { echo -e "${BLUE}[VM-TEST]${NC} $*"; }
ok()    { echo -e "${GREEN}[PASS]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --rebuild)        FORCE_REBUILD=true; shift ;;
            --shell)          SHELL_MODE=true; shift ;;
            --skip-parallel)  SKIP_PARALLEL=true; shift ;;
            --custom-kernel)  CUSTOM_KERNEL=true; shift ;;
            --ram)            VM_RAM="$2"; VM_RAM_SET=true; shift 2 ;;
            --timeout)        VM_TIMEOUT="$2"; shift 2 ;;
            --8gb)            VM_RAM="8G"; VM_TIMEOUT="300"; shift ;;
            --4gb)            VM_RAM="4G"; VM_TIMEOUT="240"; shift ;;
            --2gb)            VM_RAM="2G"; VM_TIMEOUT="200"; shift ;;
            --help|-h)
                head -15 "$0"
                echo ""
                echo "Size presets:"
                echo "  --8gb            8GB RAM, 4 CPUs, 300s timeout (full perf test)"
                echo "  --4gb            4GB RAM, 4 CPUs, 240s timeout"
                echo "  --2gb            2GB RAM, 4 CPUs, 200s timeout"
                echo "  --ram SIZE       Custom RAM size (e.g., 8G, 4096M)"
                echo "  --timeout SECS   Custom timeout"
                exit 0
                ;;
            *) warn "Unknown: $1"; shift ;;
        esac
    done
}

check_prereqs() {
    if ! command -v qemu-system-x86_64 &>/dev/null; then
        fail "qemu-system-x86_64 not found. Install qemu-base."
        exit 1
    fi

    if $CUSTOM_KERNEL; then
        # Use the custom kernel built by scripts/build-custom-kernel.sh
        local custom_dir="$SCRIPT_DIR/.kernel/out"
        KERNEL=$(find "$custom_dir/boot" -name 'vmlinuz-*' 2>/dev/null | head -1)
        if [[ -z "$KERNEL" || ! -f "$KERNEL" ]]; then
            fail "Custom kernel not found in $custom_dir/boot/"
            fail "Run ./scripts/build-custom-kernel.sh first"
            exit 1
        fi
        # Determine the custom kernel version for module path
        CUSTOM_KVER=$(basename "$KERNEL" | sed 's/vmlinuz-//')
        CUSTOM_MODDIR="$custom_dir/lib/modules/$CUSTOM_KVER"
        if [[ ! -d "$CUSTOM_MODDIR" ]]; then
            warn "Custom kernel modules not found: $CUSTOM_MODDIR"
            warn "Will use host kernel modules as fallback"
        fi
        log "Using custom kernel: $KERNEL"
        log "Custom kernel version: $CUSTOM_KVER"
    else
        # Find host kernel
        KERNEL=""
        for k in "/boot/vmlinuz-$(uname -r)" "/boot/vmlinuz-$(uname -r | sed 's/\.[0-9]*-.*//')"; do
            if [[ -f "$k" ]]; then
                KERNEL="$k"
                break
            fi
        done
        if [[ -z "$KERNEL" ]]; then
            KERNEL=$(find /boot -name 'vmlinuz-*' -maxdepth 1 2>/dev/null | head -1)
        fi

        if [[ ! -f "$KERNEL" ]]; then
            fail "Kernel not found: $KERNEL"
            fail "Use --custom-kernel to boot with a custom-built kernel"
            exit 1
        fi
    fi

    if [[ ! -f "$MODULE" ]]; then
        fail "Module not found: $MODULE"
        log "Run ./build-kmod.sh first"
        exit 1
    fi

    # Check that the module vermagic matches the kernel we'll boot
    local host_kver
    host_kver="$(uname -r)"
    local module_vermagic
    module_vermagic=$(modinfo "$MODULE" 2>/dev/null | grep -oP 'vermagic:\s+\K.*' | head -1 || echo "unknown")
    local module_kver
    module_kver=$(echo "$module_vermagic" | sed 's/ .*//' | head -1 || echo "unknown")

    if $CUSTOM_KERNEL; then
        # With custom kernel, the module vermagic must match the custom kernel
        if [[ "$module_kver" != "$CUSTOM_KVER" ]]; then
            warn "Module vermagic ($module_kver) doesn't match custom kernel ($CUSTOM_KVER)"
            log "Rebuilding module against custom kernel..."
            if ! ./build-kmod.sh custom 2>&1; then
                fail "Module rebuild against custom kernel failed"
                exit 1
            fi
            module_vermagic=$(modinfo "$MODULE" 2>/dev/null | grep -oP 'vermagic:\s+\K.*' | head -1 || echo "unknown")
            module_kver=$(echo "$module_vermagic" | sed 's/ .*//' | head -1 || echo "unknown")
        fi
        log "Module vermagic: $module_kver (custom kernel)"
    else
        # With host kernel, the module must match the host kernel version
        if [[ "$module_kver" != "$host_kver" ]]; then
            warn "Module vermagic ($module_kver) doesn't match host kernel ($host_kver)"
            log "Rebuilding module against host kernel..."
            if ! ./build-kmod.sh build 2>&1; then
                fail "Module rebuild failed"
                exit 1
            fi
            module_vermagic=$(modinfo "$MODULE" 2>/dev/null | grep -oP 'vermagic:\s+\K.*' | head -1 || echo "unknown")
            module_kver=$(echo "$module_vermagic" | sed 's/ .*//' | head -1 || echo "unknown")
            if [[ "$module_kver" != "$host_kver" ]]; then
                fail "Module still doesn't match host kernel after rebuild"
                fail "Module: $module_kver, Host: $host_kver"
                fail "You may need to install kernel headers: sudo pacman -S linux-headers"
                exit 1
            fi
        fi
        log "Module vermagic: $module_kver (host kernel)"
    fi

    ok "Prerequisites met"
    log "  Kernel:  $KERNEL"
    log "  Module:  $MODULE"
    log "  QEMU:    $(qemu-system-x86_64 --version | head -1)"
}

build_rootfs() {
    # Check if rootfs needs rebuilding
    local needs_rebuild=false
    if $FORCE_REBUILD; then
        needs_rebuild=true
    elif [[ ! -f "$ROOTFS" ]]; then
        needs_rebuild=true
    else
        # Check if module or test binaries are newer than the rootfs
        if [[ "$MODULE" -nt "$ROOTFS" ]]; then
            log "Module is newer than rootfs — rebuilding"
            needs_rebuild=true
        fi
        for bin in tests/kernel/test_stress_concurrent tests/kernel/test_stress_unload \
                   tests/kernel/test_stress_pressure tests/kernel/test_cpu_overhead \
                   tests/kernel/test_drain_restore tests/kernel/test_perf_harness \
                   tests/kernel/test_transparent_e2e \
                   tests/kernel/kselftest-minimem.sh tests/kernel/minimem_e2e_test.sh \
                   tests/kernel/test_transparent_compress.sh tests/kernel/test_transparent_kprobe.sh \
                   tests/kernel/test_scanner_roundtrip.sh; do
            if [[ -f "$SCRIPT_DIR/$bin" ]] && [[ "$SCRIPT_DIR/$bin" -nt "$ROOTFS" ]]; then
                log "Test binary $bin is newer than rootfs — rebuilding"
                needs_rebuild=true
                break
            fi
        done
    fi

    if ! $needs_rebuild; then
        log "Using existing rootfs: $ROOTFS"
        return 0
    fi

    # Use Alpine minirootfs as base — reliable, small, has busybox
    log "Building VM rootfs..."

    rm -rf "$VM_DIR/rootfs"
    mkdir -p "$VM_DIR/rootfs"

    local root="$VM_DIR/rootfs"
    local alpine_url="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.3-x86_64.tar.gz"
    local alpine_tar="$VM_DIR/alpine-rootfs.tar.gz"

    # Download Alpine minirootfs if needed
    if [[ ! -f "$alpine_tar" ]]; then
        log "Downloading Alpine minirootfs..."
        curl -fsSL "$alpine_url" -o "$alpine_tar" || {
            fail "Cannot download Alpine minirootfs from $alpine_url"
            exit 1
        }
    fi

    # Extract Alpine minirootfs
    tar -xzf "$alpine_tar" -C "$root" 2>/dev/null
    ok "Alpine minirootfs extracted"

    # Create /tmp, /sys, /proc if missing
    mkdir -p "$root"/{tmp,sys,proc,dev}

    # Create init script (KERNEL_VERSION is expanded at rootfs build time)
    local INIT_KVER
    if $CUSTOM_KERNEL && [[ -n "${CUSTOM_KVER:-}" ]]; then
        INIT_KVER="$CUSTOM_KVER"
    else
        INIT_KVER="$(uname -r)"
    fi

    cat > "$root/init" << 'INITEOF'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t debugfs debugfs /sys/kernel/debug
mount -t devtmpfs devtmpfs /dev

echo "========================================"
echo "MiniMem VM Test Environment"
echo "========================================"
echo "Kernel: $(uname -r)"
echo "CPUs:   $(nproc)"
echo "RAM:    $(free -m | head -2 | tail -1 | awk '{print $2}') MB"
echo ""

# Load LZ4 dependency
insmod /lib/modules/$(uname -r)/kernel/lib/lz4/lz4_compress.ko 2>/dev/null || \
    modprobe lz4_compress 2>/dev/null || \
    echo "lz4_compress: not loaded (may be built-in)"

# Load MiniMem module
echo "Loading minimem module..."
insmod /minimem.ko 2>/dev/null
if [ $? -ne 0 ]; then
    # Try force-loading if vermagic mismatch
    echo "Normal insmod failed, trying with --force..."
    insmod -f /minimem.ko 2>/dev/null
    if [ $? -ne 0 ]; then
        # Try modprobe as last resort
        modprobe minimem 2>/dev/null
        if [ $? -ne 0 ]; then
            echo "FATAL: Failed to load minimem module"
            echo "FATAL: Check dmesg below"
            dmesg | tail -20
            echo "PANIC_MARKER: module_load_failed"
            # Don't power off — let the host see the error
            exec /bin/sh
        fi
    fi
fi

echo "Module loaded successfully"
echo ""

ensure_module_loaded() {
    if [ ! -d /sys/kernel/minimem ]; then
        echo "Re-loading minimem module..."
        insmod /minimem.ko 2>/dev/null || insmod -f /minimem.ko 2>/dev/null || modprobe minimem 2>/dev/null
        sleep 1
        if [ ! -d /sys/kernel/minimem ]; then
            echo "WARNING: Failed to reload minimem module"
            return 1
        fi
    fi
    return 0
}

# Run kselftest first (before any state modifications)
echo "=== Running kselftest ==="
sh /kselftest-minimem.sh
echo ""

# Run end-to-end transparent compression test
echo "=== Running E2E test ==="
sh /minimem_e2e_test.sh
echo ""

ensure_module_loaded

# Run transparent compression test (compress → PTE replace → fault → verify)
echo "=== Running transparent compression test ==="
sh /test_transparent_compress.sh
echo ""

# Run kprobe E2E test (scanner compress → fault handler decompress)
echo "=== Running kprobe transparent compression test ==="
sh /test_transparent_kprobe.sh
echo ""

ensure_module_loaded

# Run scanner roundtrip test (full pipeline: scanner + fault handler + data integrity)
if [ -x /test_scanner_roundtrip.sh ]; then
    echo "=== Running scanner roundtrip test ==="
    sh /test_scanner_roundtrip.sh
    echo ""
fi

ensure_module_loaded

# Run CPU overhead test
if [ -x /test_cpu_overhead ]; then
    echo "=== Running CPU overhead measurement ==="
    /test_cpu_overhead
    echo ""
fi

ensure_module_loaded

# Run drain-and-restore test
if [ -x /test_drain_restore ]; then
    echo "=== Running drain-and-restore test ==="
    /test_drain_restore
    echo ""
fi

ensure_module_loaded

# Run stress tests if available
if [ -x /test_stress_concurrent ]; then
    echo "=== Running concurrent fault stress test ==="
    /test_stress_concurrent 8 4 2
    echo ""
fi

ensure_module_loaded

if [ -x /test_stress_unload ]; then
    echo "=== Running module unload safety test ==="
    /test_stress_unload
    echo ""
fi

ensure_module_loaded

if [ -x /test_stress_pressure ]; then
    echo "=== Running memory pressure stress test ==="
    /test_stress_pressure 16 2
    echo ""
fi

ensure_module_loaded

# Run performance harness (latency, throughput, concurrency, activity, overhead, re-compression)
if [ -x /test_perf_harness ]; then
    AVAIL_KB=$(cat /proc/meminfo | grep MemAvailable | awk '{print $2}')
    if [ "$AVAIL_KB" -gt 4000000 ]; then
        echo "=== Running performance harness (full test, ${AVAIL_KB}KB available) ==="
        /test_perf_harness --pages 2048 --threads 8 --rounds 3 --csv /tmp/minimem-perf-results.csv
    elif [ "$AVAIL_KB" -gt 1000000 ]; then
        echo "=== Running performance harness (medium test, ${AVAIL_KB}KB available) ==="
        /test_perf_harness --pages 512 --threads 4 --rounds 2 --csv /tmp/minimem-perf-results.csv
    else
        echo "=== Running performance harness (quick test, ${AVAIL_KB}KB available) ==="
        /test_perf_harness --quick --csv /tmp/minimem-perf-results.csv
    fi
    echo ""
    if [ -f /tmp/minimem-perf-results.csv ]; then
        echo "=== Performance results ==="
        cat /tmp/minimem-perf-results.csv
        echo ""
    fi
fi

# Check for kernel errors after kselftest
echo "=== Kernel log after kselftest ==="
dmesg | grep -iE "minimem" | tail -5 || true
echo ""

# Re-load module if kselftest unloaded it
ensure_module_loaded

# Check sysfs
echo "=== Sysfs stats ==="
for f in /sys/kernel/minimem/*; do
    echo "  $(basename $f): $(cat $f 2>/dev/null || echo 'N/A')"
done
echo ""

# Check debugfs
if [ -d /sys/kernel/debug/minimem ]; then
    echo "=== Debugfs available ==="
    ls -la /sys/kernel/debug/minimem/ 2>/dev/null || true
    echo ""
else
    echo "WARNING: debugfs not available"
    echo ""
fi

# Run baseline benchmark
echo "=== Baseline benchmark (memcpy roundtrip) ==="
echo "baseline" > /sys/kernel/debug/minimem/bench 2>/dev/null || echo "bench write failed"
sleep 1
echo "Stats after baseline:"
cat /sys/kernel/debug/minimem/stats 2>/dev/null || echo "stats read failed"
echo ""

# Run serial benchmark
echo "=== Serial benchmark (compress → store → decompress) ==="
echo "serial" > /sys/kernel/debug/minimem/bench 2>/dev/null || echo "bench write failed"
sleep 1
echo "Stats after serial:"
cat /sys/kernel/debug/minimem/stats 2>/dev/null || echo "stats read failed"
echo ""

# Check zswap
echo "=== Zswap state ==="
echo "  zswap_pages: $(cat /sys/kernel/minimem/zswap_pages 2>/dev/null || echo N/A)"
echo "  zswap_bytes: $(cat /sys/kernel/minimem/zswap_bytes 2>/dev/null || echo N/A)"
echo "  zswap_saved: $(cat /sys/kernel/minimem/zswap_saved 2>/dev/null || echo N/A)"
echo ""

# Run parallel benchmark
echo "=== Parallel benchmark (workqueue cluster decompression) ==="
echo "parallel" > /sys/kernel/debug/minimem/bench 2>/dev/null || echo "bench write failed"
sleep 2
echo "Stats after parallel:"
cat /sys/kernel/debug/minimem/stats 2>/dev/null || echo "stats read failed"
echo ""

# Test max_pool_pages sysfs knob
echo "=== max_pool_pages test ==="
echo "  Initial max_pool_pages: $(cat /sys/kernel/minimem/max_pool_pages)"
echo "0" > /sys/kernel/minimem/max_pool_pages
echo "  After setting to 0 (unlimited): $(cat /sys/kernel/minimem/max_pool_pages)"
echo "1" > /sys/kernel/minimem/max_pool_pages
echo "  After setting to 1: $(cat /sys/kernel/minimem/max_pool_pages)"
echo "0" > /sys/kernel/minimem/max_pool_pages
echo "  Reset to unlimited: $(cat /sys/kernel/minimem/max_pool_pages)"
echo ""

# Test min_savings_pct sysfs knob
echo "=== min_savings_pct test ==="
echo "  Initial min_savings_pct: $(cat /sys/kernel/minimem/min_savings_pct)"
echo "50" > /sys/kernel/minimem/min_savings_pct
echo "  After setting to 50: $(cat /sys/kernel/minimem/min_savings_pct)"
echo "13" > /sys/kernel/minimem/min_savings_pct
echo "  Reset to 13: $(cat /sys/kernel/minimem/min_savings_pct)"
echo ""

# Test scanner sysfs knobs (scanner is disabled by default)
echo "=== Scanner sysfs test ==="
echo "  scanner_enabled: $(cat /sys/kernel/minimem/scanner_enabled)"
echo "  scanner_interval_ms: $(cat /sys/kernel/minimem/scanner_interval_ms)"
echo "  scanner_pages_scanned: $(cat /sys/kernel/minimem/scanner_pages_scanned)"
echo "  scanner_pages_idle: $(cat /sys/kernel/minimem/scanner_pages_idle)"
echo "  scanner_pages_compressed: $(cat /sys/kernel/minimem/scanner_pages_compressed)"
echo "  scanner_pages_skipped: $(cat /sys/kernel/minimem/scanner_pages_skipped)"
echo ""

# Final check
echo "=== Final sysfs ==="
for f in /sys/kernel/minimem/*; do
    echo "  $(basename $f): $(cat $f 2>/dev/null || echo 'N/A')"
done
echo ""

# Check for kernel errors
echo "=== Kernel log check ==="
dmesg | grep -iE "bug|panic|oops|minimem" | tail -10 || true
echo ""

# Unload module (kselftest may have already unloaded it)
echo "Unloading minimem module..."
rmmod minimem 2>/dev/null || true
sleep 1
if [ -d /sys/kernel/minimem ]; then
    echo "WARNING: sysfs still exists after rmmod"
else
    echo "Module unloaded"
fi
echo ""

echo "TESTS_COMPLETE_MARKER"
echo "All tests passed!"

# Clean shutdown
sync
poweroff -f
INITEOF
    chmod +x "$root/init"

    # Alpine rootfs already has busybox and all essential tools

    # Copy the kernel module (put in / to avoid tmpfs overwriting)
    cp "$MODULE" "$root/minimem.ko"
    chmod +x "$root/minimem.ko"

    # Copy kselftest script
    cp "$SCRIPT_DIR/tests/kernel/kselftest-minimem.sh" "$root/kselftest-minimem.sh"
    chmod +x "$root/kselftest-minimem.sh"

    # Copy e2e test script
    cp "$SCRIPT_DIR/tests/kernel/minimem_e2e_test.sh" "$root/minimem_e2e_test.sh"
    chmod +x "$root/minimem_e2e_test.sh"

    # Copy transparent compression test
    cp "$SCRIPT_DIR/tests/kernel/test_transparent_compress.sh" "$root/test_transparent_compress.sh"
    chmod +x "$root/test_transparent_compress.sh"

    # Copy kprobe E2E test (new — tests scanner + kprobe fault handler)
    cp "$SCRIPT_DIR/tests/kernel/test_transparent_kprobe.sh" "$root/test_transparent_kprobe.sh"
    chmod +x "$root/test_transparent_kprobe.sh"

    # Copy static E2E test binary
    cp "$SCRIPT_DIR/tests/kernel/test_transparent_e2e" "$root/test_transparent_e2e"
    chmod +x "$root/test_transparent_e2e"

    # Copy stress test binaries (if built)
    for bin in test_stress_concurrent test_stress_pressure test_stress_unload \
               test_cpu_overhead test_drain_restore test_perf_harness; do
        if [ -f "$SCRIPT_DIR/tests/kernel/$bin" ]; then
            cp "$SCRIPT_DIR/tests/kernel/$bin" "$root/$bin"
            chmod +x "$root/$bin"
        fi
    done

    # Copy scanner roundtrip test script
    if [ -f "$SCRIPT_DIR/tests/kernel/test_scanner_roundtrip.sh" ]; then
        cp "$SCRIPT_DIR/tests/kernel/test_scanner_roundtrip.sh" "$root/test_scanner_roundtrip.sh"
        chmod +x "$root/test_scanner_roundtrip.sh"
    fi

    # Copy kernel modules we need (lz4_compress)
    local kmod_dir
    local target_kver
    if $CUSTOM_KERNEL && [[ -n "${CUSTOM_KVER:-}" ]]; then
        # Use custom kernel's module directory
        kmod_dir="$CUSTOM_MODDIR"
        target_kver="$CUSTOM_KVER"
    else
        kmod_dir="/lib/modules/$(uname -r)"
        target_kver="$(uname -r)"
    fi
    mkdir -p "$root/lib/modules/$target_kver"

    # lz4_compress may be built-in or a module
    mkdir -p "$root/lib/modules/$target_kver"

    if ls "$kmod_dir/kernel/lib/lz4/"*.ko.zst 2>/dev/null; then
        mkdir -p "$root/lib/modules/$target_kver/kernel/lib/lz4"
        for f in "$kmod_dir/kernel/lib/lz4/"*.ko.zst; do
            zstd -d -f "$f" -o "$root/lib/modules/$target_kver/kernel/lib/lz4/$(basename "${f%.zst}")" 2>/dev/null || \
                cp "$f" "$root/lib/modules/$target_kver/kernel/lib/lz4/"
        done
        if [ -f "$kmod_dir/modules.dep" ]; then
            cp "$kmod_dir/modules.dep" "$root/lib/modules/$target_kver/"
            cp "$kmod_dir/modules.dep.bin" "$root/lib/modules/$target_kver/" 2>/dev/null || true
        fi
        ok "LZ4 module copied (decompressed)"
    elif ls "$kmod_dir/kernel/lib/lz4/"*.ko 2>/dev/null; then
        mkdir -p "$root/lib/modules/$target_kver/kernel/lib/lz4"
        cp "$kmod_dir/kernel/lib/lz4/"*.ko "$root/lib/modules/$target_kver/kernel/lib/lz4/"
        if [ -f "$kmod_dir/modules.dep" ]; then
            cp "$kmod_dir/modules.dep" "$root/lib/modules/$target_kver/"
            cp "$kmod_dir/modules.dep.bin" "$root/lib/modules/$target_kver/" 2>/dev/null || true
        fi
        ok "LZ4 module copied"
    else
        log "LZ4 appears built-in to kernel"
    fi

    # For custom kernel: also include all custom kernel modules in the rootfs
    if $CUSTOM_KERNEL && [[ -d "$CUSTOM_MODDIR" ]]; then
        log "Copying custom kernel modules to rootfs..."
        # Copy essential modules (but skip the huge ones we don't need for initrd)
        mkdir -p "$root/lib/modules/$CUSTOM_KVER"
        # Copy modules.dep and other metadata
        for f in modules.dep modules.dep.bin modules.alias modules.alias.bin \
                 modules.symbols modules.builtin modules.builtin.modinfo modules.order; do
            if [ -f "$CUSTOM_MODDIR/$f" ]; then
                cp "$CUSTOM_MODDIR/$f" "$root/lib/modules/$CUSTOM_KVER/"
            fi
        done
        # We don't copy all modules to keep the initrd small — the custom kernel
        # has CONFIG_MINIMEM=y built-in, so no external module needed for that.
        # However, the MiniMem loadable module is still needed for the actual
        # compression logic (the built-in part is only the fault handler stub).
        ok "Custom kernel module metadata copied"
    fi

    # Create cpio archive
    (cd "$root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9) > "$ROOTFS"

    local size
    size=$(du -h "$ROOTFS" | cut -f1)
    ok "Rootfs built: $ROOTFS ($size)"
}

run_vm_test() {
    local kernel_cmdline="console=ttyS0 panic=-1 rdinit=/init root=/dev/ram0"

    # Check if host has enough memory for the VM
    local host_mem_kb
    host_mem_kb=$(grep MemAvailable /proc/meminfo 2>/dev/null | awk '{print $2}')
    if [[ -n "$host_mem_kb" ]]; then
        # Convert VM_RAM to KB for comparison (handle M and G suffixes)
        local vm_ram_kb
        case "$VM_RAM" in
            *G) vm_ram_kb=$((${VM_RAM%G} * 1024 * 1024)) ;;
            *M) vm_ram_kb=$((${VM_RAM%M} * 1024)) ;;
            *)  vm_ram_kb=$((VM_RAM * 1024)) ;;
        esac
        # Need at least 1.5x VM RAM available to avoid host OOM
        local required_kb=$((vm_ram_kb * 3 / 2))
        if [[ "$host_mem_kb" -lt "$required_kb" ]] 2>/dev/null; then
            warn "Host only has $((host_mem_kb / 1024))MB available"
            warn "VM requests ${VM_RAM} — need at least $((required_kb / 1024))MB free"
            warn "Tests may be slow or fail due to host memory pressure"
            echo ""
            read -t 10 -p "Continue anyway? [y/N] " cont 2>/dev/null || cont="n"
            [[ "${cont,,}" == "y" ]] || { fail "Aborted due to insufficient host memory"; return 1; }
        fi
    fi

    local qemu_extra_args=""
    if $CUSTOM_KERNEL && [[ -n "${CUSTOM_KVER:-}" ]]; then
        # When using custom kernel, also pass the custom kernel's modules as a virtiofs
        # or simply rely on the initrd containing the needed modules
        log "Using custom kernel: $CUSTOM_KVER"
        log "  MINIMEM is built into the kernel (CONFIG_MINIMEM=y)"
    fi

    if $SHELL_MODE; then
        # Interactive shell — just boot and drop to shell
        local kvm_arg=""
        [ -w /dev/kvm ] 2>/dev/null && kvm_arg="-enable-kvm"

        qemu-system-x86_64 \
            -m "$VM_RAM" \
            -smp "$VM_CPUS" \
            -kernel "$KERNEL" \
            -initrd "$ROOTFS" \
            -append "$kernel_cmdline" \
            -nographic \
            -no-reboot \
            -monitor none \
            -serial stdio \
            $kvm_arg
        return $?
    fi

    # Automated test — capture output, timeout
    log "Booting VM (timeout: ${VM_TIMEOUT}s)..."
    log "  RAM: $VM_RAM, CPUs: $VM_CPUS"

    # Auto-increase timeout for large RAM or no-KVM
    local effective_timeout="$VM_TIMEOUT"
    if [ ! -w /dev/kvm ] 2>/dev/null; then
        effective_timeout=$((VM_TIMEOUT * 2))
        log "  No KVM — doubling timeout to ${effective_timeout}s"
    fi

    local output_file="$VM_DIR/vm_output.log"
    local timeout_file="$VM_DIR/timeout_flag"

    rm -f "$timeout_file"

    # Run VM with timeout
    # Use --foreground so Ctrl-C propagates properly
    # Use -enable-kvm if available for much faster boot, fall back to TCG
    local kvm_arg=""
    if [ -w /dev/kvm ] 2>/dev/null; then
        kvm_arg="-enable-kvm"
        log "KVM acceleration available"
    else
        log "KVM not available — using TCG (will be slower)"
    fi

    log "Starting QEMU..."
    timeout --foreground "${effective_timeout}s" \
        qemu-system-x86_64 \
        -m "$VM_RAM" \
        -smp "$VM_CPUS" \
        -kernel "$KERNEL" \
        -initrd "$ROOTFS" \
        -append "$kernel_cmdline" \
        -nographic \
        -no-reboot \
        -monitor none \
        -serial stdio \
        $kvm_arg \
        2>&1 | tee "$output_file" || true

    if [[ ! -f "$output_file" ]]; then
        fail "No VM output captured"
        return 1
    fi

    # Analyze results
    echo ""
    log "=== Analyzing VM output ==="

    local failed=false

    if grep -q "PANIC_MARKER: module_load_failed" "$output_file"; then
        fail "Module failed to load in VM"
        failed=true
    fi

    if grep -qi "kernel panic" "$output_file" && ! grep -q "panic=-1" "$output_file" 2>/dev/null; then
        fail "Kernel panic detected in VM"
        failed=true
    fi

    if ! grep -q "TESTS_COMPLETE_MARKER" "$output_file"; then
        warn "VM did not reach test completion — may have timed out or crashed"
        failed=true
    fi

    # ── Extract compatibility report ──
    echo ""
    log "=== Compatibility Report ==="
    if grep -q "compat_report" "$output_file"; then
        grep -A1 "compat_report" "$output_file" | grep -E "kernel_version|kallsyms|kprobes|required_symbols|swp_type|pte_mkwrite|kernel_patches|page_idle|zsmalloc|folio_rmap|fail_count|overall" | while read -r line; do
            echo "  $line"
        done
    else
        echo "  (not captured in VM output)"
    fi

    # ── Extract benchmark results ──
    local baseline_ns
    baseline_ns=$(grep -oP 'compress_ns_total \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    serial_decompress=$(grep -oP 'decompress_ns_total \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    parallel_pages=$(grep -oP 'parallel_pages \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    zswap_pages=$(grep -oP 'zswap_pages \K[0-9]+' "$output_file" | tail -1 || echo "N/A")

    echo ""
    log "=== Core Results ==="
    echo "  Module load:        $(grep -q 'Module loaded successfully' "$output_file" && echo 'OK' || echo 'FAILED')"
    echo "  Module unload:      $(grep -q 'Module unloaded' "$output_file" && echo 'OK' || echo 'FAILED')"
    echo "  Baseline test:      $(grep -q 'baseline' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Serial test:        $(grep -q 'serial' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Parallel test:       $(grep -q 'parallel' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Compress ns total:  $baseline_ns"
    echo "  Decompress ns:     $serial_decompress"
    echo "  Parallel pages:    $parallel_pages"
    echo "  Final zswap pages: $zswap_pages"
    echo "  max_pool_pages:    $(grep 'max_pool_pages' "$output_file" | tail -1 | awk '{print $NF}' || echo 'N/A')"
    echo "  min_savings_pct:   $(grep 'min_savings_pct' "$output_file" | tail -1 | awk '{print $NF}' || echo 'N/A')"
    echo "  scanner_enabled:   $(grep 'scanner_enabled' "$output_file" | tail -1 | awk '{print $NF}' || echo 'N/A')"
    echo ""

    # ── Extract performance harness results ──
    echo ""
    log "=== Performance Harness Results ==="
    if grep -q "Performance Harness Results" "$output_file"; then
        grep -A2 "Test 1: Compression & Decompression Latency" "$output_file" | grep -E "decompress|PASS|WARN|FAIL|p50|p99" | head -5
        echo "  ---"
        grep -A5 "Test 5: Memory Overhead" "$output_file" | grep -E "Compression ratio|Savings|Overhead|PASS|WARN|FAIL" | head -5
        echo "  ---"
        grep -A3 "Test 6: Scanner-driven Re-compression" "$output_file" | grep -E "PASS|FAIL|SKIP|scanner" | head -3
    else
        echo "  (performance harness did not run)"
    fi

    # ── Extract latency numbers from sysfs ──
    echo ""
    log "=== Latency Summary ==="
    local comp_avg decomp_avg comp_count decomp_count
    comp_avg=$(grep -oP 'compress_avg_ns \K[0-9]+' "$output_file" | tail -1 || echo "N/A")
    decomp_avg=$(grep -oP 'decompress_avg_ns \K[0-9]+' "$output_file" | tail -1 || echo "N/A")
    comp_count=$(grep -oP 'compress_count \K[0-9]+' "$output_file" | tail -1 || echo "0")
    decomp_count=$(grep -oP 'decompress_count \K[0-9]+' "$output_file" | tail -1 || echo "0")
    echo "  Compress avg:    ${comp_avg} ns"
    echo "  Decompress avg:  ${decomp_avg} ns"
    echo "  Compress count:  ${comp_count}"
    echo "  Decompress count: ${decomp_count}"

    if [[ "$decomp_avg" != "N/A" ]] && [[ "$decomp_avg" -gt 0 ]] 2>/dev/null; then
        local decomp_us
        decomp_us=$(echo "scale=1; $decomp_avg / 1000" | bc 2>/dev/null || echo "N/A")
        echo "  Decompress latency: ${decomp_us} us"
        if [[ "$decomp_avg" -lt 10000 ]]; then
            echo "  Verdict: EXCELLENT (< 10us)"
        elif [[ "$decomp_avg" -lt 50000 ]]; then
            echo "  Verdict: GOOD (< 50us)"
        elif [[ "$decomp_avg" -lt 100000 ]]; then
            echo "  Verdict: ACCEPTABLE (< 100us)"
        else
            echo "  Verdict: SLOW (> 100us)"
        fi
    fi

    # ── Extract throughput ──
    local bytes_saved
    bytes_saved=$(grep -oP 'bytes_saved \K[0-9]+' "$output_file" | tail -1 || echo "0")
    pool_pages=$(grep -oP 'pool_pages \K[0-9]+' "$output_file" | tail -1 || echo "0")
    echo ""
    log "=== Throughput & Efficiency ==="
    echo "  Bytes saved:     ${bytes_saved}"
    echo "  Pool pages:      ${pool_pages}"
    if [[ "$zswap_pages" -gt 0 ]] && [[ "$bytes_saved" -gt 0 ]] 2>/dev/null; then
        local ratio
        ratio=$(echo "scale=2; $bytes_saved / ($zswap_pages * 4096) * 100" | bc 2>/dev/null || echo "N/A")
        echo "  Savings ratio:   ${ratio}%"
    fi

    # ── Scanner stats ──
    echo ""
    log "=== Scanner Stats ==="
    local scan_scanned scan_compressed scan_skipped scan_idle
    scan_scanned=$(grep -oP 'scanner_pages_scanned \K[0-9]+' "$output_file" | tail -1 || echo "0")
    scan_compressed=$(grep -oP 'scanner_pages_compressed \K[0-9]+' "$output_file" | tail -1 || echo "0")
    scan_skipped=$(grep -oP 'scanner_pages_skipped \K[0-9]+' "$output_file" | tail -1 || echo "0")
    scan_idle=$(grep -oP 'scanner_pages_idle \K[0-9]+' "$output_file" | tail -1 || echo "0")
    echo "  Pages scanned:     ${scan_scanned}"
    echo "  Pages idle:        ${scan_idle}"
    echo "  Pages compressed:  ${scan_compressed}"
    echo "  Pages skipped:     ${scan_skipped}"

    # ── Test pass/fail counts ──
    echo ""
    local kselftest_pass
    kselftest_pass=$(grep '^PASS: ' "$output_file" 2>/dev/null | wc -l)
    local kselftest_fail
    kselftest_fail=$(grep '^FAIL: ' "$output_file" 2>/dev/null | wc -l)
    local kselftest_skip
    kselftest_skip=$(grep '^SKIP: ' "$output_file" 2>/dev/null | wc -l)
    echo "  kselftest PASS:    $kselftest_pass"
    echo "  kselftest FAIL:    $kselftest_fail"
    echo "  kselftest SKIP:    $kselftest_skip"
    local e2e_pass
    e2e_pass=$(grep '^PASS: ' "$output_file" 2>/dev/null | wc -l)
    local e2e_fail
    e2e_fail=$(grep '^FAIL: ' "$output_file" 2>/dev/null | wc -l)
    local e2e_skip
    e2e_skip=$(grep '^SKIP: ' "$output_file" 2>/dev/null | wc -l)
    local e2e_only_pass
    e2e_only_pass=$((e2e_pass - kselftest_pass))
    local e2e_only_fail
    e2e_only_fail=$((e2e_fail - kselftest_fail))
    local e2e_only_skip
    e2e_only_skip=$((e2e_skip - kselftest_skip))
    echo "  e2e PASS:          ${e2e_only_pass:-0}"
    echo "  e2e FAIL:          ${e2e_only_fail:-0}"
    echo "  e2e SKIP:          ${e2e_only_skip:-0}"
    echo ""

    # ── Bad page state / oops check ──
    local bad_page oops bug
    bad_page=$(grep -c "Bad page state" "$output_file" 2>/dev/null || echo "0")
    oops=$(grep -c "Oops:" "$output_file" 2>/dev/null || echo "0")
    bug=$(grep -ci "BUG:" "$output_file" 2>/dev/null || echo "0")
    if [[ "$bad_page" -gt 0 ]] || [[ "$oops" -gt 0 ]] || [[ "$bug" -gt 0 ]]; then
        fail "Kernel issues detected: Bad page state=$bad_page Oops=$oops BUG=$bug"
        failed=true
    else
        ok "No kernel issues (Bad page state, Oops, BUG)"
    fi

    echo ""
    log "=== VM Configuration ==="
    echo "  RAM: $VM_RAM"
    echo "  CPUs: $VM_CPUS"
    echo "  Timeout: ${VM_TIMEOUT}s"

    if grep -q "All tests passed!" "$output_file"; then
        ok "All VM tests passed!"
        return 0
    elif $failed; then
        fail "VM tests failed"
        return 1
    else
        warn "VM tests incomplete"
        return 1
    fi
}

main() {
    parse_args "$@"

    echo ""
    log "MiniMem VM Test Harness (QEMU)"
    log "==============================="
    echo ""

    check_prereqs
    build_rootfs

    if $SHELL_MODE; then
        log "Booting VM in interactive shell mode..."
        log "Type 'poweroff' to exit the VM"
        echo ""
        run_vm_test
        exit $?
    fi

    run_vm_test
    local result=$?

    echo ""
    if [[ $result -eq 0 ]]; then
        ok "VM test suite complete — all tests passed"
    else
        fail "VM test suite had failures"
        log "Full output: $VM_DIR/vm_output.log"
    fi

    return $result
}

main "$@"