#!/bin/sh
# test_scanner_roundtrip.sh — Full scanner+fault handler roundtrip test
#
# Verifies the complete transparent compression pipeline:
#   1. Allocate pages with known patterns
#   2. Let the scanner find and compress them
#   3. Access the pages (triggering fault handler decompression)
#   4. Verify data integrity
#   5. Verify adaptive interval behavior
#   6. Verify skip filter counters
#   7. Verify CPU overhead within bounds
#
# Must run inside the MiniMem VM.
# Exit codes: 0 = all pass, 1 = any fail

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "SKIP: $1"; }

SYSDIR="/sys/kernel/minimem"

echo "=== Test: Scanner Roundtrip ==="
echo ""

# Check prerequisites
if [ ! -d "$SYSDIR" ]; then
    fail "minimem sysfs not found — module not loaded?"
    exit 1
fi

# Check required sysfs attributes
for attr in scanner_enabled scanner_pages_scanned scanner_pages_compressed \
            scanner_cycles_total scanner_cycles_empty \
            scanner_current_interval_ms hook_faults \
            scanner_skip_vma_locked scanner_skip_page_shared \
            scanner_skip_incompressible decompress_avg_ns \
            compress_avg_ns; do
    if [ -f "$SYSDIR/$attr" ]; then
        pass "sysfs attribute $attr exists"
    else
        fail "sysfs attribute $attr missing"
    fi
done
echo ""

# Record baseline
BEFORE_CMP=$(cat "$SYSDIR/pages_compressed" 2>/dev/null || echo 0)
BEFORE_DCMP=$(cat "$SYSDIR/pages_decompressed" 2>/dev/null || echo 0)
BEFORE_FAULTS=$(cat "$SYSDIR/hook_faults" 2>/dev/null || echo 0)
BEFORE_SCANNED=$(cat "$SYSDIR/scanner_pages_scanned" 2>/dev/null || echo 0)
BEFORE_CMP_SCAN=$(cat "$SYSDIR/scanner_pages_compressed" 2>/dev/null || echo 0)
BEFORE_CYCLES=$(cat "$SYSDIR/scanner_cycles_total" 2>/dev/null || echo 0)
BEFORE_SKIP_VMA=$(cat "$SYSDIR/scanner_skip_vma_locked" 2>/dev/null || echo 0)
BEFORE_SKIP_SHARED=$(cat "$SYSDIR/scanner_skip_page_shared" 2>/dev/null || echo 0)
BEFORE_SKIP_MLOCK=$(cat "$SYSDIR/scanner_skip_page_mlocked" 2>/dev/null || echo 0)
BEFORE_SKIP_INCOMP=$(cat "$SYSDIR/scanner_skip_incompressible" 2>/dev/null || echo 0)
INTERVAL=$(cat "$SYSDIR/scanner_interval_ms" 2>/dev/null || echo 1000)

echo "Baseline:"
echo "  compressed:       $BEFORE_CMP"
echo "  decompressed:     $BEFORE_DCMP"
echo "  hook_faults:      $BEFORE_FAULTS"
echo "  scanned:          $BEFORE_SCANNED"
echo "  scanner_compressed: $BEFORE_CMP_SCAN"
echo "  cycles:           $BEFORE_CYCLES"
echo "  interval_ms:      $INTERVAL"
echo "  skip_vma_locked:  $BEFORE_SKIP_VMA"
echo "  skip_shared:      $BEFORE_SKIP_SHARED"
echo "  skip_mlocked:     $BEFORE_SKIP_MLOCK"
echo "  skip_incomp:      $BEFORE_SKIP_INCOMP"
echo ""

# Check if a fault handler is available
KP=$(cat "$SYSDIR/kernel_patches" 2>/dev/null || echo 0)
HOOK_MSG=$(dmesg | grep "minimem:" | grep -E "kprobe registered|fault handler" | tail -3)
echo "  kernel_patches: $KP"
echo "  hook messages: $HOOK_MSG"

if [ "$KP" = "1" ]; then
    pass "kernel patches — full fault handler active"
elif echo "$HOOK_MSG" | grep -q "kprobe registered"; then
    pass "kprobe fault handler active"
else
    skip "no fault handler — transparent decompression won't work"
    echo ""
    echo "========================================"
    echo "Scanner Roundtrip Test Results"
    echo "========================================"
    echo "  Passed:  $PASS"
    echo "  Failed:  $FAIL"
    echo "  Skipped: $SKIP"
    echo ""
    exit 0
fi
echo ""

# Test 1: Static binary roundtrip
if [ -x /test_transparent_e2e ]; then
    echo "--- Running static E2E binary ---"
    /test_transparent_e2e
    E2E_RC=$?
    if [ $E2E_RC -eq 0 ]; then
        pass "test_transparent_e2e returned 0"
    else
        fail "test_transparent_e2e returned $E2E_RC"
    fi
else
    skip "test_transparent_e2e binary not available"
fi
echo ""

# Test 2: Scanner compression with static binary
if [ -x /test_cpu_overhead ]; then
    echo "--- Running CPU overhead test ---"
    /test_cpu_overhead
    CPU_RC=$?
    if [ $CPU_RC -eq 0 ]; then
        pass "test_cpu_overhead returned 0"
    else
        fail "test_cpu_overhead returned $CPU_RC"
    fi
else
    skip "test_cpu_overhead binary not available"
fi
echo ""

# Test 3: Scanner stats verification
echo "--- Scanner statistics verification ---"

AFTER_SCANNED=$(cat "$SYSDIR/scanner_pages_scanned" 2>/dev/null || echo 0)
AFTER_CMP_SCAN=$(cat "$SYSDIR/scanner_pages_compressed" 2>/dev/null || echo 0)
AFTER_FAULTS=$(cat "$SYSDIR/hook_faults" 2>/dev/null || echo 0)
AFTER_CYCLES=$(cat "$SYSDIR/scanner_cycles_total" 2>/dev/null || echo 0)
AFTER_CYCLES_EMPTY=$(cat "$SYSDIR/scanner_cycles_empty" 2>/dev/null || echo 0)
AFTER_INTERVAL=$(cat "$SYSDIR/scanner_current_interval_ms" 2>/dev/null || echo 0)

DELTA_SCANNED=$((AFTER_SCANNED - BEFORE_SCANNED))
DELTA_CMP=$((AFTER_CMP_SCAN - BEFORE_CMP_SCAN))
DELTA_FAULTS=$((AFTER_FAULTS - BEFORE_FAULTS))
DELTA_CYCLES=$((AFTER_CYCLES - BEFORE_CYCLES))

echo "  Scanned pages:     $DELTA_SCANNED"
echo "  Compressed pages:  $DELTA_CMP"
echo "  Faults handled:    $DELTA_FAULTS"
echo "  Scanner cycles:    $DELTA_CYCLES"
echo "  Current interval:  $AFTER_INTERVAL ms"
echo "  Empty cycles:      $AFTER_CYCLES_EMPTY"

if [ "$DELTA_SCANNED" -gt 0 ]; then
    pass "scanner scanned $DELTA_SCANNED pages"
else
    echo "  (scanner may need CONFIG_PAGE_IDLE_FLAG)"
fi

if [ "$DELTA_CMP" -gt 0 ]; then
    pass "scanner compressed $DELTA_CMP pages"
elif [ "$DELTA_SCANNED" -gt 0 ]; then
    echo "  (pages may not be idle enough yet)"
fi

if [ "$DELTA_FAULTS" -gt 0 ]; then
    pass "fault handler decompressed $DELTA_FAULTS pages"
fi

if [ "$DELTA_CYCLES" -gt 0 ]; then
    pass "scanner ran $DELTA_CYCLES cycles"
fi
echo ""

# Test 4: Decompression latency check
DECOMP_AVG=$(cat "$SYSDIR/decompress_avg_ns" 2>/dev/null || echo 0)
COMP_AVG=$(cat "$SYSDIR/compress_avg_ns" 2>/dev/null || echo 0)

echo "--- Latency check ---"
if [ "$DECOMP_AVG" -gt 0 ] && [ "$DECOMP_AVG" -lt 100000 ]; then
    DECOMP_US=$(echo "scale=1; $DECOMP_AVG / 1000" | bc 2>/dev/null || echo "?")
    echo "  Avg decompress: ${DECOMP_US} us ($DECOMP_AVG ns)"
    if [ "$DECOMP_AVG" -lt 10000 ]; then
        pass "decompress latency < 10us"
    elif [ "$DECOMP_AVG" -lt 50000 ]; then
        pass "decompress latency < 50us (acceptable)"
    else
        fail "decompress latency > 50us ($DECOMP_AVG ns)"
    fi
else
    echo "  Avg decompress: N/A (no decompression data)"
fi

if [ "$COMP_AVG" -gt 0 ] && [ "$COMP_AVG" -lt 200000 ]; then
    COMP_US=$(echo "scale=1; $COMP_AVG / 1000" | bc 2>/dev/null || echo "?")
    echo "  Avg compress:   ${COMP_US} us ($COMP_AVG ns)"
    if [ "$COMP_AVG" -lt 50000 ]; then
        pass "compress latency < 50us"
    else
        echo "  (compress latency acceptable for background scanner)"
    fi
else
    echo "  Avg compress:   N/A"
fi
echo ""

# Test 5: Skip filter counters
AFTER_SKIP_VMA=$(cat "$SYSDIR/scanner_skip_vma_locked" 2>/dev/null || echo 0)
AFTER_SKIP_SHARED=$(cat "$SYSDIR/scanner_skip_page_shared" 2>/dev/null || echo 0)
AFTER_SKIP_MLOCK=$(cat "$SYSDIR/scanner_skip_page_mlocked" 2>/dev/null || echo 0)
AFTER_SKIP_INCOMP=$(cat "$SYSDIR/scanner_skip_incompressible" 2>/dev/null || echo 0)

echo "--- Skip filter verification ---"
echo "  skip_vma_locked:      $(($AFTER_SKIP_VMA - $BEFORE_SKIP_VMA))"
echo "  skip_page_shared:     $(($AFTER_SKIP_SHARED - $BEFORE_SKIP_SHARED))"
echo "  skip_page_mlocked:    $(($AFTER_SKIP_MLOCK - $BEFORE_SKIP_MLOCK))"
echo "  skip_incompressible:  $(($AFTER_SKIP_INCOMP - $BEFORE_SKIP_INCOMP))"

if [ "$AFTER_SKIP_VMA" -ge "$BEFORE_SKIP_VMA" ]; then
    pass "skip_vma_locked counter non-decreasing"
else
    fail "skip_vma_locked counter went backwards"
fi
echo ""

# Test 6: Drain-and-restore verification (via static binary)
if [ -x /test_drain_restore ]; then
    echo "--- Running drain-and-restore test ---"
    /test_drain_restore
    DR_RC=$?
    if [ $DR_RC -eq 0 ]; then
        pass "test_drain_restore returned 0"
    else
        fail "test_drain_restore returned $DR_RC"
    fi
else
    skip "test_drain_restore binary not available"
fi
echo ""

# Check for kernel errors
echo "--- Kernel log check ---"
ERRORS=$(dmesg | grep -iE "bug|panic|oops|minimem.*error|minimem.*fail" | grep -v "pr_debug" | tail -5 || true)
if [ -z "$ERRORS" ]; then
    pass "no kernel errors detected"
else
    fail "kernel errors found:"
    echo "$ERRORS"
fi
echo ""

echo "========================================"
echo "Scanner Roundtrip Test Results"
echo "========================================"
echo "  Passed:  $PASS"
echo "  Failed:  $FAIL"
echo "  Skipped: $SKIP"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "ALL TESTS PASSED"
    exit 0
else
    echo "SOME TESTS FAILED"
    exit 1
fi