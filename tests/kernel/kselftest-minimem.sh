#!/bin/sh
# kselftest-minimem.sh — Kernel module self-test for MiniMem
#
# Runs inside the QEMU VM. Tests module load/unload, sysfs,
# debugfs, PTE markers, compression, scanner, and sysfs knobs.
#
# Exit codes: 0 = all pass, 1 = any fail

PASS=0
FAIL=0
SKIP=0

pass() {
    PASS=$((PASS + 1))
    echo "PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    echo "FAIL: $1"
}

skip() {
    SKIP=$((SKIP + 1))
    echo "SKIP: $1"
}

SYSDIR="/sys/kernel/minimem"
DEBUGDIR="/sys/kernel/debug/minimem"

# ============================================================
# Test 1: Module load
# ============================================================
echo "=== Test 1: Module load ==="
if [ -d "$SYSDIR" ]; then
    pass "sysfs already exists (module pre-loaded)"
else
    # Try to load LZ4 dependency
    insmod /lib/modules/$(uname -r)/kernel/lib/lz4/lz4_compress.ko 2>/dev/null || \
        modprobe lz4_compress 2>/dev/null || true

    if insmod /minimem.ko 2>/dev/null && [ -d "$SYSDIR" ]; then
        pass "module loaded successfully"
    else
        fail "module load failed"
        echo "=== TESTS ABORTED ==="
        exit 1
    fi
fi

# ============================================================
# Test 2: Sysfs attributes exist
# ============================================================
echo ""
echo "=== Test 2: Sysfs attributes ==="
REQUIRED_ATTRS="pages_compressed pages_decompressed bytes_saved
compress_count decompress_count compress_ns_total decompress_ns_total
compress_avg_ns decompress_avg_ns
zswap_pages zswap_bytes zswap_saved pool_pages
parallel_clusters parallel_pages
scanner_enabled scanner_interval_ms min_savings_pct
scanner_pages_scanned scanner_pages_idle scanner_pages_compressed scanner_pages_skipped
hook_faults kernel_patches max_pool_pages parallel_mode
per_process_stats per_process_top_n stats_summary"

for attr in $REQUIRED_ATTRS; do
    if [ -f "$SYSDIR/$attr" ]; then
        val=$(cat "$SYSDIR/$attr" 2>/dev/null)
        pass "sysfs $attr = $val"
    else
        fail "sysfs $attr missing"
    fi
done

# ============================================================
# Test 3: Debugfs exists
# ============================================================
echo ""
echo "=== Test 3: Debugfs ==="
if [ -d "$DEBUGDIR" ]; then
    for f in bench compress stats pte_test; do
        if [ -e "$DEBUGDIR/$f" ]; then
            pass "debugfs $f exists"
        else
            fail "debugfs $f missing"
        fi
    done
else
    fail "debugfs directory missing"
fi

# ============================================================
# Test 4: PTE marker roundtrip
# ============================================================
echo ""
echo "=== Test 4: PTE marker roundtrip ==="
if [ -w "$DEBUGDIR/pte_test" ]; then
    if echo "encode 42" > "$DEBUGDIR/pte_test" 2>/dev/null; then
        pass "PTE encode succeeded"
    else
        fail "PTE encode failed"
    fi

    if echo "encode 0" > "$DEBUGDIR/pte_test" 2>/dev/null; then
        pass "PTE encode 0 succeeded"
    else
        fail "PTE encode 0 failed"
    fi

    if echo "encode 1099511627775" > "$DEBUGDIR/pte_test" 2>/dev/null; then
        pass "PTE encode max index succeeded"
    else
        fail "PTE encode max index failed"
    fi
else
    skip "pte_test not writable"
fi

# ============================================================
# Test 5: Compression roundtrip via debugfs
# ============================================================
echo ""
echo "=== Test 5: Compression roundtrip ==="
if [ -w "$DEBUGDIR/compress" ]; then
    if echo "1" > "$DEBUGDIR/compress" 2>/dev/null; then
        saved_before=$(cat "$SYSDIR/zswap_saved" 2>/dev/null)
        echo "1" > "$DEBUGDIR/compress" 2>/dev/null
        saved_after=$(cat "$SYSDIR/zswap_saved" 2>/dev/null)
        if [ "$saved_after" -gt "$saved_before" ] 2>/dev/null; then
            pass "compression saved bytes ($saved_before -> $saved_after)"
        else
            fail "compression did not save bytes"
        fi
    else
        fail "compress write failed"
    fi
else
    skip "compress not writable"
fi

# ============================================================
# Test 6: Benchmark via debugfs
# ============================================================
echo ""
echo "=== Test 6: Benchmark ==="
if [ -w "$DEBUGDIR/bench" ]; then
    echo "baseline" > "$DEBUGDIR/bench" 2>/dev/null
    sleep 1
    baseline_ns=$(awk '{print $1}' "$SYSDIR/bench_baseline_ns" 2>/dev/null)
    if [ "$baseline_ns" != "0" ] && [ -n "$baseline_ns" ]; then
        pass "baseline benchmark ran ($baseline_ns ns)"
    else
        fail "baseline benchmark did not run"
    fi

    echo "serial" > "$DEBUGDIR/bench" 2>/dev/null
    sleep 1
    serial_ns=$(awk '{print $1}' "$SYSDIR/bench_serial_ns" 2>/dev/null)
    if [ "$serial_ns" != "0" ] && [ -n "$serial_ns" ]; then
        pass "serial benchmark ran ($serial_ns ns)"
    else
        fail "serial benchmark did not run"
    fi
else
    skip "bench not writable"
fi

# ============================================================
# Test 7: Sysfs knob defaults and write
# ============================================================
echo ""
echo "=== Test 7: Sysfs knobs ==="

# max_pool_pages
val=$(cat "$SYSDIR/max_pool_pages" 2>/dev/null)
if [ "$val" = "0" ]; then
    pass "max_pool_pages default is 0 (unlimited)"
else
    fail "max_pool_pages default is $val (expected 0)"
fi

echo "100" > "$SYSDIR/max_pool_pages" 2>/dev/null
val=$(cat "$SYSDIR/max_pool_pages" 2>/dev/null)
if [ "$val" = "100" ]; then
    pass "max_pool_pages set to 100"
else
    fail "max_pool_pages set failed ($val)"
fi
echo "0" > "$SYSDIR/max_pool_pages" 2>/dev/null

# min_savings_pct
val=$(cat "$SYSDIR/min_savings_pct" 2>/dev/null)
if [ "$val" = "13" ]; then
    pass "min_savings_pct default is 13"
else
    fail "min_savings_pct default is $val (expected 13)"
fi

echo "50" > "$SYSDIR/min_savings_pct" 2>/dev/null
val=$(cat "$SYSDIR/min_savings_pct" 2>/dev/null)
if [ "$val" = "50" ]; then
    pass "min_savings_pct set to 50"
else
    fail "min_savings_pct set failed ($val)"
fi
echo "13" > "$SYSDIR/min_savings_pct" 2>/dev/null

# scanner_enabled
val=$(cat "$SYSDIR/scanner_enabled" 2>/dev/null)
if [ "$val" = "0" ]; then
    pass "scanner_enabled default is 0 (disabled)"
else
    fail "scanner_enabled default is $val (expected 0)"
fi

echo "1" > "$SYSDIR/scanner_enabled" 2>/dev/null
val=$(cat "$SYSDIR/scanner_enabled" 2>/dev/null)
if [ "$val" = "1" ]; then
    pass "scanner_enabled set to 1"
else
    fail "scanner_enabled set failed ($val)"
fi
echo "0" > "$SYSDIR/scanner_enabled" 2>/dev/null

# scanner_interval_ms
val=$(cat "$SYSDIR/scanner_interval_ms" 2>/dev/null)
if [ "$val" = "1000" ]; then
    pass "scanner_interval_ms default is 1000"
else
    fail "scanner_interval_ms default is $val (expected 1000)"
fi

echo "500" > "$SYSDIR/scanner_interval_ms" 2>/dev/null
val=$(cat "$SYSDIR/scanner_interval_ms" 2>/dev/null)
if [ "$val" = "500" ]; then
    pass "scanner_interval_ms set to 500"
else
    fail "scanner_interval_ms set failed ($val)"
fi
echo "1000" > "$SYSDIR/scanner_interval_ms" 2>/dev/null

# parallel_mode
val=$(cat "$SYSDIR/parallel_mode" 2>/dev/null)
case "$val" in
    auto*) pass "parallel_mode default is auto" ;;
    2)     pass "parallel_mode default is 2 (auto)" ;;
    *)     fail "parallel_mode default is '$val' (expected auto/2)" ;;
esac

echo "0" > "$SYSDIR/parallel_mode" 2>/dev/null
val=$(cat "$SYSDIR/parallel_mode" 2>/dev/null)
case "$val" in
    disabled*) pass "parallel_mode set to disabled" ;;
    0)         pass "parallel_mode set to 0" ;;
    *)         fail "parallel_mode set to 0 failed ($val)" ;;
esac

echo "1" > "$SYSDIR/parallel_mode" 2>/dev/null
val=$(cat "$SYSDIR/parallel_mode" 2>/dev/null)
case "$val" in
    enabled*) pass "parallel_mode set to enabled" ;;
    1)        pass "parallel_mode set to 1" ;;
    *)        fail "parallel_mode set to 1 failed ($val)" ;;
esac

echo "2" > "$SYSDIR/parallel_mode" 2>/dev/null
val=$(cat "$SYSDIR/parallel_mode" 2>/dev/null)
case "$val" in
    auto*) pass "parallel_mode set back to auto" ;;
    2)     pass "parallel_mode set back to 2" ;;
    *)     fail "parallel_mode set back to auto failed ($val)" ;;
esac

echo "auto" > "$SYSDIR/parallel_mode" 2>/dev/null
val=$(cat "$SYSDIR/parallel_mode" 2>/dev/null)
case "$val" in
    auto*) pass "parallel_mode accepts 'auto' string" ;;
    *)     fail "parallel_mode 'auto' string failed ($val)" ;;
esac

# ============================================================
# Test 7b: Per-process stats sysfs knobs
# ============================================================
echo ""
echo "=== Test 7b: Per-process stats ==="

# per_process_stats defaults to 0
val=$(cat "$SYSDIR/per_process_stats" 2>/dev/null)
if [ "$val" = "0" ]; then
    pass "per_process_stats default is 0 (disabled)"
else
    fail "per_process_stats default is $val (expected 0)"
fi

# stats_summary should say disabled when off
val=$(cat "$SYSDIR/stats_summary" 2>/dev/null)
if echo "$val" | grep -q "disabled"; then
    pass "stats_summary shows disabled when per_process_stats=0"
else
    fail "stats_summary should show disabled ($val)"
fi

# Enable per-process stats
echo "1" > "$SYSDIR/per_process_stats" 2>/dev/null
val=$(cat "$SYSDIR/per_process_stats" 2>/dev/null)
if [ "$val" = "1" ]; then
    pass "per_process_stats enabled"
else
    fail "per_process_stats enable failed ($val)"
fi

# per_process_top_n default
val=$(cat "$SYSDIR/per_process_top_n" 2>/dev/null)
if [ "$val" = "5" ]; then
    pass "per_process_top_n default is 5"
else
    fail "per_process_top_n default is $val (expected 5)"
fi

# Set top_n to 10
echo "10" > "$SYSDIR/per_process_top_n" 2>/dev/null
val=$(cat "$SYSDIR/per_process_top_n" 2>/dev/null)
if [ "$val" = "10" ]; then
    pass "per_process_top_n set to 10"
else
    fail "per_process_top_n set to 10 failed ($val)"
fi

# Reject invalid top_n
echo "0" > "$SYSDIR/per_process_top_n" 2>/dev/null
val=$(cat "$SYSDIR/per_process_top_n" 2>/dev/null)
if [ "$val" = "10" ]; then
    pass "per_process_top_n rejects 0"
else
    fail "per_process_top_n should reject 0 ($val)"
fi

echo "21" > "$SYSDIR/per_process_top_n" 2>/dev/null
val=$(cat "$SYSDIR/per_process_top_n" 2>/dev/null)
if [ "$val" = "10" ]; then
    pass "per_process_top_n rejects 21 (>20 max)"
else
    fail "per_process_top_n should reject 21 ($val)"
fi

# Reset top_n
echo "5" > "$SYSDIR/per_process_top_n" 2>/dev/null

# stats_summary should now show tracked_processes
val=$(cat "$SYSDIR/stats_summary" 2>/dev/null)
if echo "$val" | grep -q "tracked_processes"; then
    pass "stats_summary shows tracked_processes when enabled"
else
    fail "stats_summary should show tracked_processes ($val)"
fi

# Disable per_process_stats (should clear all entries)
echo "0" > "$SYSDIR/per_process_stats" 2>/dev/null
val=$(cat "$SYSDIR/per_process_stats" 2>/dev/null)
if [ "$val" = "0" ]; then
    pass "per_process_stats disabled"
else
    fail "per_process_stats disable failed ($val)"
fi

# Debugfs per_process should exist
if [ -e "$DEBUGDIR/per_process" ]; then
    pass "debugfs per_process exists"
else
    fail "debugfs per_process missing"
fi

# ============================================================
# Test 8: Module unload
# ============================================================
echo ""
echo "=== Test 8: Module unload ==="
if rmmod minimem 2>/dev/null; then
    pass "module unloaded successfully"
else
    fail "module unload failed"
fi

if [ ! -d "$SYSDIR" ]; then
    pass "sysfs cleaned up after unload"
else
    fail "sysfs still exists after unload"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "========================================"
echo "MiniMem kselftest results"
echo "========================================"
echo "  Passed:  $PASS"
echo "  Failed:  $FAIL"
echo "  Skipped: $SKIP"
echo "  Total:   $((PASS + FAIL + SKIP))"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi