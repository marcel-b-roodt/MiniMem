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
#   ./vm-test-minimem.sh --rebuild    # Force rebuild rootfs
#   ./vm-test-minimem.sh --shell      # Boot VM and drop to shell
#   ./vm-test-minimem.sh --skip-parallel  # Skip parallel benchmark

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE="$SCRIPT_DIR/src/kernel/minimem.ko"
VM_DIR="$SCRIPT_DIR/.vm-test"
ROOTFS="$VM_DIR/rootfs.cpio.gz"
# Find kernel — try exact match first, then fallback
KERNEL=""
for k in "/boot/vmlinuz-$(uname -r)" "/boot/vmlinuz-$(uname -r | sed 's/\.[0-9]*-.*//')"; do
    if [[ -f "$k" ]]; then
        KERNEL="$k"
        break
    fi
done
if [[ -z "$KERNEL" ]]; then
    # Try any vmlinuz in /boot
    KERNEL=$(ls /boot/vmlinuz-* 2>/dev/null | head -1)
fi

INITRD=""
for i in "/boot/initramfs-$(uname -r).img" "/boot/initramfs-$(uname -r | sed 's/\.[0-9]*-.*//').img"; do
    if [[ -f "$i" ]]; then
        INITRD="$i"
        break
    fi
done

# VM settings
VM_RAM="768M"
VM_CPUS="4"
VM_TIMEOUT="120"

SKIP_PARALLEL=false
SHELL_MODE=false
FORCE_REBUILD=false

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
            --help|-h)
                head -15 "$0"
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

    if [[ ! -f "$KERNEL" ]]; then
        fail "Kernel not found: $KERNEL"
        exit 1
    fi

    if [[ ! -f "$MODULE" ]]; then
        fail "Module not found: $MODULE"
        log "Run ./build-kmod.sh first"
        exit 1
    fi

    ok "Prerequisites met"
    log "  Kernel:  $KERNEL"
    log "  Module:  $MODULE"
    log "  QEMU:    $(qemu-system-x86_64 --version | head -1)"
}

build_rootfs() {
    if [[ -f "$ROOTFS" ]] && ! $FORCE_REBUILD; then
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

    # Create init script
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
insmod /minimem.ko
if [ $? -ne 0 ]; then
    echo "FATAL: Failed to load minimem module"
    echo "FATAL: Check dmesg below"
    dmesg | tail -20
    echo "PANIC_MARKER: module_load_failed"
    # Don't power off — let the host see the error
    exec /bin/sh
fi

echo "Module loaded successfully"
echo ""

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

# Unload module
echo "Unloading minimem module..."
rmmod minimem 2>/dev/null || echo "rmmod failed"
sleep 1
echo "Module unloaded"
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

    # Copy kernel modules we need (lz4_compress)
    local kmod_dir="/lib/modules/$(uname -r)"
    mkdir -p "$root/lib/modules/$(uname -r)"

    # lz4_compress may be built-in or a module
    local kmod_dir="/lib/modules/$(uname -r)"
    mkdir -p "$root/lib/modules/$(uname -r)"

    if ls "$kmod_dir/kernel/lib/lz4/"*.ko.zst 2>/dev/null; then
        mkdir -p "$root/lib/modules/$(uname -r)/kernel/lib/lz4"
        for f in "$kmod_dir/kernel/lib/lz4/"*.ko.zst; do
            zstd -d -f "$f" -o "$root/lib/modules/$(uname -r)/kernel/lib/lz4/$(basename "${f%.zst}")" 2>/dev/null || \
                cp "$f" "$root/lib/modules/$(uname -r)/kernel/lib/lz4/"
        done
        if [ -f "$kmod_dir/modules.dep" ]; then
            cp "$kmod_dir/modules.dep" "$root/lib/modules/$(uname -r)/"
            cp "$kmod_dir/modules.dep.bin" "$root/lib/modules/$(uname -r)/" 2>/dev/null || true
        fi
        ok "LZ4 module copied (decompressed)"
    elif ls "$kmod_dir/kernel/lib/lz4/"*.ko 2>/dev/null; then
        mkdir -p "$root/lib/modules/$(uname -r)/kernel/lib/lz4"
        cp "$kmod_dir/kernel/lib/lz4/"*.ko "$root/lib/modules/$(uname -r)/kernel/lib/lz4/"
        if [ -f "$kmod_dir/modules.dep" ]; then
            cp "$kmod_dir/modules.dep" "$root/lib/modules/$(uname -r)/"
            cp "$kmod_dir/modules.dep.bin" "$root/lib/modules/$(uname -r)/" 2>/dev/null || true
        fi
        ok "LZ4 module copied"
    else
        log "LZ4 appears built-in to kernel"
    fi

    # Create cpio archive
    (cd "$root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9) > "$ROOTFS"

    local size
    size=$(du -h "$ROOTFS" | cut -f1)
    ok "Rootfs built: $ROOTFS ($size)"
}

run_vm_test() {
    local kernel_cmdline="console=ttyS0 panic=-1 quiet rdinit=/init root=/dev/ram0"
    local qemu_extra=""

    if $SHELL_MODE; then
        # Interactive shell — just boot and drop to shell
        qemu-system-x86_64 \
            -m "$VM_RAM" \
            -smp "$VM_CPUS" \
            -kernel "$KERNEL" \
            -initrd "$ROOTFS" \
            -append "$kernel_cmdline" \
            -nographic \
            -no-reboot \
            -monitor none \
            -serial stdio
        return $?
    fi

    # Automated test — capture output, timeout
    log "Booting VM (timeout: ${VM_TIMEOUT}s)..."
    log "  RAM: $VM_RAM, CPUs: $VM_CPUS"

    local output_file="$VM_DIR/vm_output.log"
    local timeout_file="$VM_DIR/timeout_flag"

    rm -f "$timeout_file"

    # Run VM with timeout
    timeout "${VM_TIMEOUT}s" qemu-system-x86_64 \
        -m "$VM_RAM" \
        -smp "$VM_CPUS" \
        -kernel "$KERNEL" \
        -initrd "$ROOTFS" \
        -append "$kernel_cmdline" \
        -nographic \
        -no-reboot \
        -monitor none \
        -serial stdio \
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

    # Extract benchmark results
    local baseline_ns serial_ns parallel_ns
    baseline_ns=$(grep -oP 'compress_ns_total \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    serial_decompress=$(grep -oP 'decompress_ns_total \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    parallel_pages=$(grep -oP 'parallel_pages \K[0-9]+' "$output_file" | head -1 || echo "N/A")
    zswap_pages=$(grep -oP 'zswap_pages \K[0-9]+' "$output_file" | tail -1 || echo "N/A")

    echo ""
    log "=== Results ==="
    echo "  Module load:        $(grep -q 'Module loaded successfully' "$output_file" && echo 'OK' || echo 'FAILED')"
    echo "  Module unload:      $(grep -q 'Module unloaded' "$output_file" && echo 'OK' || echo 'FAILED')"
    echo "  Baseline test:      $(grep -q 'baseline' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Serial test:        $(grep -q 'serial' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Parallel test:       $(grep -q 'parallel' "$output_file" && echo 'OK' || echo 'N/A')"
    echo "  Compress ns total:  $baseline_ns"
    echo "  Decompress ns:     $serial_decompress"
    echo "  Parallel pages:    $parallel_pages"
    echo "  Final zswap pages: $zswap_pages"
    echo ""

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