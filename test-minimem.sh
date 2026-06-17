#!/bin/bash
# test-minimem.sh — Safe MiniMem kernel module test harness
#
# Tests the MiniMem module progressively:
#   Phase 1: Build verification
#   Phase 2: Module load/unload (no compression)
#   Phase 3: Sysfs stats check
#   Phase 4: Debugfs baseline benchmark (memcpy only)
#   Phase 5: Serial compress/decompress benchmark
#   Phase 6: Parallel cluster decompression benchmark
#   Phase 7: Cleanup
#
# Usage:
#   ./test-minimem.sh              # Run all phases
#   ./test-minimem.sh --phase 4    # Run only phase 4
#   ./test-minimem.sh --skip-parallel  # Skip phase 6 (safest for bare metal)
#   ./test-minimem.sh --dry-run    # Show what would be done without loading
#
# SAFETY: Each phase validates the module is healthy before proceeding.
# If any phase fails, the script attempts cleanup and exits.
# The parallel decompression test (phase 6) is the riskiest because
# it uses workqueues with zsmalloc. Use --skip-parallel on bare metal
# until you trust the module.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE="$SCRIPT_DIR/src/kernel/minimem.ko"
MODULE_NAME="minimem"
SYSFS_DIR="/sys/kernel/minimem"
DEBUGFS_DIR="/sys/kernel/debug/minimem"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SKIP_PARALLEL=false
DRY_RUN=false
PHASE=""
PHASES=(1 2 3 4 5 6 7)

log()   { echo -e "${BLUE}[MINIMEM]${NC} $*"; }
ok()    { echo -e "${GREEN}[PASS]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-parallel) SKIP_PARALLEL=true; shift ;;
            --dry-run)       DRY_RUN=true; shift ;;
            --phase)         PHASE="$2"; shift 2 ;;
            --help|-h)
                head -20 "$0"
                exit 0
                ;;
            *) warn "Unknown argument: $1"; shift ;;
        esac
    done

    if [[ -n "$PHASE" ]]; then
        PHASES=("$PHASE")
    fi

    if $SKIP_PARALLEL; then
        PHASES=(1 2 3 4 5 7)
    fi
}

check_sudo() {
    if ! sudo -n true 2>/dev/null; then
        warn "This script requires sudo for insmod/rmmod"
        warn "You may be prompted for your password."
    fi
}

check_module_built() {
    if [[ ! -f "$MODULE" ]]; then
        fail "Module not found: $MODULE"
        log "Run ./build-kmod.sh first"
        return 1
    fi

    local ver
    ver=$(modinfo -F version "$MODULE" 2>/dev/null || echo "unknown")
    ok "Module built: $MODULE (version: $ver)"

    local depends
    depends=$(modinfo -F depends "$MODULE" 2>/dev/null || echo "none")
    log "Module depends on: $depends"

    local kver
    kver=$(modinfo -F vermagic "$MODULE" 2>/dev/null | awk '{print $1}' || echo "unknown")
    local running
    running=$(uname -r)
    if [[ "$kver" != "$running" ]]; then
        fail "Module built for kernel $kver but running $running"
        return 1
    fi
    ok "Kernel version match: $kver"
}

check_deps() {
    local dep
    dep=$(modinfo -F depends "$MODULE" 2>/dev/null)
    if [[ -n "$dep" ]]; then
        for mod in $(echo "$dep" | tr ',' ' '); do
            if ! lsmod | grep -q "^${mod}"; then
                log "Loading dependency: $mod"
                if $DRY_RUN; then
                    log "[DRY RUN] Would run: sudo modprobe $mod"
                else
                    sudo modprobe "$mod" || {
                        fail "Cannot load dependency: $mod"
                        return 1
                    }
                fi
            fi
            ok "Dependency loaded: $mod"
        done
    fi
}

check_not_loaded() {
    if lsmod | grep -q "^${MODULE_NAME}"; then
        warn "Module already loaded, unloading first..."
        if ! $DRY_RUN; then
            sudo rmmod "$MODULE_NAME" 2>/dev/null || true
            sleep 1
        fi
    fi
}

check_debugfs() {
    if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
        log "Mounting debugfs..."
        if ! $DRY_RUN; then
            sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
        fi
    fi
    if mountpoint -q /sys/kernel/debug 2>/dev/null; then
        ok "debugfs mounted at /sys/kernel/debug"
    else
        warn "debugfs not mounted — debugfs tests will be skipped"
    fi
}

# ---- Phase 1: Build verification ----
phase_1() {
    log "=== Phase 1: Build verification ==="
    check_module_built
    check_deps
    ok "Phase 1 complete"
}

# ---- Phase 2: Module load/unload ----
phase_2() {
    log "=== Phase 2: Module load/unload ==="
    check_not_loaded
    check_debugfs

    if $DRY_RUN; then
        log "[DRY RUN] Would run: sudo insmod $MODULE"
        ok "Phase 2 complete (dry run)"
        return 0
    fi

    log "Loading module..."
    sudo dmesg -C
    sudo insmod "$MODULE"

    sleep 1

    if ! lsmod | grep -q "^${MODULE_NAME}"; then
        fail "Module not loaded after insmod"
        sudo dmesg | tail -20
        return 1
    fi
    ok "Module loaded"

    local dmesg_out
    dmesg_out=$(sudo dmesg | grep -i minimem || true)
    log "Kernel messages:"
    echo "$dmesg_out" | head -5

    log "Unloading module..."
    sudo rmmod "$MODULE_NAME"
    sleep 1

    if lsmod | grep -q "^${MODULE_NAME}"; then
        fail "Module still loaded after rmmod"
        return 1
    fi
    ok "Module unloaded cleanly"

    sudo dmesg | grep -i minimem | tail -3
    ok "Phase 2 complete"
}

# ---- Phase 3: Sysfs stats ----
phase_3() {
    log "=== Phase 3: Sysfs stats check ==="

    if $DRY_RUN; then
        log "[DRY RUN] Would load module and check sysfs"
        return 0
    fi

    check_not_loaded
    sudo insmod "$MODULE"
    sleep 1

    local expected_attrs=(
        pages_compressed
        pages_decompressed
        bytes_saved
        compress_count
        decompress_count
        compress_ns_total
        decompress_ns_total
        decompress_avg_ns
        compress_avg_ns
        zswap_pages
        zswap_bytes
        zswap_saved
        parallel_clusters
        parallel_pages
    )

    local missing=0
    for attr in "${expected_attrs[@]}"; do
        if [[ -f "$SYSFS_DIR/$attr" ]]; then
            local val
            val=$(cat "$SYSFS_DIR/$attr" 2>/dev/null || echo "N/A")
            ok "  $attr = $val"
        else
            fail "  $attr: missing"
            missing=$((missing + 1))
        fi
    done

    if [[ $missing -gt 0 ]]; then
        fail "Missing $missing sysfs attributes"
        return 1
    fi

    ok "Phase 3 complete — all 14 sysfs attributes present"
}

# ---- Phase 4: Baseline benchmark (memcpy only, safe) ----
phase_4() {
    log "=== Phase 4: Baseline benchmark (memcpy roundtrip) ==="

    if $DRY_RUN; then
        log "[DRY RUN] Would run: echo baseline > $DEBUGFS_DIR/bench"
        return 0
    fi

    if [[ ! -d "$DEBUGFS_DIR" ]]; then
        fail "debugfs not available"
        return 1
    fi

    log "Running baseline benchmark (32 pages, memcpy only)..."
    sudo dmesg -C
    echo "baseline" | sudo tee "$DEBUGFS_DIR/bench" > /dev/null

    sleep 1

    local dmesg_out
    dmesg_out=$(sudo dmesg | grep -i "minimem\|bug\|panic\|oops" || true)
    if echo "$dmesg_out" | grep -qi "bug\|panic\|oops"; then
        fail "Kernel error detected during baseline benchmark"
        echo "$dmesg_out"
        return 1
    fi

    local stats
    stats=$(cat "$DEBUGFS_DIR/stats" 2>/dev/null || echo "N/A")
    log "Stats after baseline:"
    echo "$stats" | grep -E "compress_count|compress_ns" || true

    ok "Phase 4 complete — baseline benchmark succeeded"
}

# ---- Phase 5: Serial compress/decompress benchmark ----
phase_5() {
    log "=== Phase 5: Serial compress/decompress benchmark ==="

    if $DRY_RUN; then
        log "[DRY RUN] Would run: echo serial > $DEBUGFS_DIR/bench"
        return 0
    fi

    if [[ ! -d "$DEBUGFS_DIR" ]]; then
        fail "debugfs not available"
        return 1
    fi

    # Reload module to reset stats
    sudo rmmod "$MODULE_NAME" 2>/dev/null || true
    sleep 1
    sudo insmod "$MODULE"
    sleep 1

    log "Running serial benchmark (32 pages, compress → zsmalloc → decompress)..."
    sudo dmesg -C
    echo "serial" | sudo tee "$DEBUGFS_DIR/bench" > /dev/null

    sleep 1

    local dmesg_out
    dmesg_out=$(sudo dmesg | grep -i "bug\|panic\|oops" || true)
    if [[ -n "$dmesg_out" ]]; then
        fail "Kernel error detected during serial benchmark"
        echo "$dmesg_out"
        return 1
    fi

    local stats
    stats=$(cat "$DEBUGFS_DIR/stats" 2>/dev/null || echo "N/A")
    log "Stats after serial:"
    echo "$stats"

    # Verify zswap stored then freed pages
    local zswap_pages
    zswap_pages=$(cat "$SYSFS_DIR/zswap_pages" 2>/dev/null || echo "0")
    if [[ "$zswap_pages" != "0" ]]; then
        warn "zswap_pages = $zswap_pages (expected 0 after serial test freed all pages)"
    else
        ok "zswap_pages = 0 (all compressed pages freed correctly)"
    fi

    ok "Phase 5 complete — serial benchmark succeeded"
}

# ---- Phase 6: Parallel cluster decompression benchmark ----
phase_6() {
    log "=== Phase 6: Parallel cluster decompression ==="

    if $SKIP_PARALLEL; then
        warn "Skipping parallel test (--skip-parallel)"
        return 0
    fi

    if $DRY_RUN; then
        log "[DRY RUN] Would run: echo parallel > $DEBUGFS_DIR/bench"
        return 0
    fi

    if [[ ! -d "$DEBUGFS_DIR" ]]; then
        fail "debugfs not available"
        return 1
    fi

    # Reload module to reset stats
    sudo rmmod "$MODULE_NAME" 2>/dev/null || true
    sleep 1
    sudo insmod "$MODULE"
    sleep 1

    log "Running parallel benchmark (32 pages, workqueue decompression)..."
    log "WARNING: This is the riskiest test. If it hangs, the module has a bug."
    log "         If no output appears within 10 seconds, Ctrl+C and reboot may be needed."

    sudo dmesg -C
    echo "parallel" | sudo tee "$DEBUGFS_DIR/bench" > /dev/null

    sleep 2

    local dmesg_out
    dmesg_out=$(sudo dmesg | grep -i "bug\|panic\|oops" || true)
    if [[ -n "$dmesg_out" ]]; then
        fail "Kernel error detected during parallel benchmark"
        echo "$dmesg_out"
        return 1
    fi

    local stats
    stats=$(cat "$DEBUGFS_DIR/stats" 2>/dev/null || echo "N/A")
    log "Stats after parallel:"
    echo "$stats"

    local clusters
    clusters=$(cat "$SYSFS_DIR/parallel_clusters" 2>/dev/null || echo "0")
    local pages
    pages=$(cat "$SYSFS_DIR/parallel_pages" 2>/dev/null || echo "0")
    ok "Parallel decompression: $clusters clusters, $pages pages"

    ok "Phase 6 complete — parallel benchmark succeeded"
}

# ---- Phase 7: Cleanup ----
phase_7() {
    log "=== Phase 7: Cleanup ==="

    if $DRY_RUN; then
        log "[DRY RUN] Would run: sudo rmmod minimem"
        return 0
    fi

    if lsmod | grep -q "^${MODULE_NAME}"; then
        log "Unloading module..."
        sudo rmmod "$MODULE_NAME"
        sleep 1

        if lsmod | grep -q "^${MODULE_NAME}"; then
            fail "Module still loaded! Manual cleanup needed."
            return 1
        fi
        ok "Module unloaded"
    else
        log "Module not loaded, nothing to clean up"
    fi

    log "Final kernel messages:"
    sudo dmesg | grep -i minimem | tail -5

    ok "Phase 7 complete — cleanup done"
}

# ---- Main ----
main() {
    parse_args "$@"

    echo ""
    log "MiniMem Kernel Module Test Harness"
    log "=================================="
    log "Module:    $MODULE"
    log "Kernel:    $(uname -r)"
    log "DRY_RUN:   $DRY_RUN"
    log "Phases:    ${PHASES[*]}"
    echo ""

    if ! $DRY_RUN; then
        check_sudo
    fi

    local phase_failed=0

    for p in "${PHASES[@]}"; do
        case "$p" in
            1) phase_1 || { phase_failed=1; break; } ;;
            2) phase_2 || { phase_failed=1; break; } ;;
            3) phase_3 || { phase_failed=1; break; } ;;
            4) phase_4 || { phase_failed=1; break; } ;;
            5) phase_5 || { phase_failed=1; break; } ;;
            6) phase_6 || { phase_failed=1; break; } ;;
            7) phase_7 ;;
            *) fail "Unknown phase: $p" ;;
        esac
        echo ""
    done

    if [[ $phase_failed -ne 0 ]]; then
        echo ""
        fail "Test failed at the phase above."
        log "Attempting cleanup..."
        sudo rmmod "$MODULE_NAME" 2>/dev/null || true
    fi

    echo ""
    log "=================================="
    if [[ $phase_failed -eq 0 ]]; then
        ok "All tests passed!"
    else
        fail "Tests failed. Check dmesg for kernel errors."
    fi
    log "=================================="

    return $phase_failed
}

main "$@"