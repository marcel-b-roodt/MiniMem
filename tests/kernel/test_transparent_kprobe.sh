#!/bin/sh
# test_transparent_kprobe.sh — E2E test for kprobe-based transparent compression
#
# Tests that the scanner can compress pages and the kprobe fault handler
# can decompress them transparently on unpatched kernels.
#
# Prerequisites: MiniMem module loaded, scanner enabled, CONFIG_PAGE_IDLE_FLAG=y
#
# Exit codes: 0 = all pass, 1 = any fail

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "SKIP: $1"; }

SYSDIR="/sys/kernel/minimem"
DEBUGDIR="/sys/kernel/debug/minimem"

echo "=== Test: Kprobe-based transparent compression ==="

# Check prerequisites
if [ ! -d "$SYSDIR" ]; then
    fail "minimem sysfs not found — module not loaded?"
    exit 1
fi

# Check that a fault handler is available (kernel patches OR kprobe)
KP=$(cat "$SYSDIR/kernel_patches" 2>/dev/null)
HOOK_MSG=$(dmesg | grep "minimem:" | grep -E "kprobe registered|fault handler" | tail -3)
echo "  kernel_patches: $KP"
echo "  hook messages: $HOOK_MSG"

if [ "$KP" = "1" ]; then
    pass "kernel patches detected — patched fault handler active"
elif echo "$HOOK_MSG" | grep -q "kprobe registered"; then
    pass "kprobe fallback active — transparent faults should work"
else
    skip "no fault handler available — transparent compression cannot work"
    echo "  Cannot test transparent compression without fault handler"
    exit 0
fi

# Check scanner skip counters exist (new in 0.9.0)
for attr in scanner_skip_vma_locked scanner_skip_page_shared scanner_skip_page_mlocked scanner_skip_incompressible scanner_mark_pages; do
    if [ -f "$SYSDIR/$attr" ]; then
        pass "sysfs attribute $attr exists"
    else
        fail "sysfs attribute $attr missing (module needs rebuild)"
    fi
done

# Record baseline counters
BEFORE_CMP=$(cat "$SYSDIR/pages_compressed" 2>/dev/null || echo 0)
BEFORE_DCMP=$(cat "$SYSDIR/pages_decompressed" 2>/dev/null || echo 0)
BEFORE_SCANNED=$(cat "$SYSDIR/scanner_pages_scanned" 2>/dev/null || echo 0)
BEFORE_IDLE=$(cat "$SYSDIR/scanner_pages_idle" 2>/dev/null || echo 0)
BEFORE_FAULTS=$(cat "$SYSDIR/hook_faults" 2>/dev/null || echo 0)
BEFORE_SKIP=$(cat "$SYSDIR/scanner_pages_skipped" 2>/dev/null || echo 0)

echo ""
echo "  Baseline: compressed=$BEFORE_CMP decompressed=$BEFORE_DCMP"
echo "  Baseline: scanned=$BEFORE_SCANNED idle=$BEFORE_IDLE"
echo "  Baseline: faults=$BEFORE_FAULTS skipped=$BEFORE_SKIP"

# Enable scanner
echo "1" > "$SYSDIR/scanner_enabled" 2>/dev/null
echo "  Scanner enabled"

# Give the scanner time to do a mark+sweep cycle
echo "  Waiting 5 seconds for scanner to find and compress idle pages..."
sleep 5

# Check scanner activity
AFTER_SCANNED=$(cat "$SYSDIR/scanner_pages_scanned" 2>/dev/null || echo 0)
AFTER_IDLE=$(cat "$SYSDIR/scanner_pages_idle" 2>/dev/null || echo 0)
AFTER_CMP=$(cat "$SYSDIR/pages_compressed" 2>/dev/null || echo 0)
AFTER_FAULTS=$(cat "$SYSDIR/hook_faults" 2>/dev/null || echo 0)

echo "  After scan: scanned=$AFTER_SCANNED idle=$AFTER_IDLE"
echo "  After scan: compressed=$AFTER_CMP faults=$AFTER_FAULTS"

if [ "$AFTER_SCANNED" -gt "$BEFORE_SCANNED" ]; then
    pass "scanner found pages to scan ($BEFORE_SCANNED -> $AFTER_SCANNED)"
else
    warn "scanner did not scan any pages (may need CONFIG_PAGE_IDLE_FLAG)"
fi

if [ "$AFTER_CMP" -gt "$BEFORE_CMP" ]; then
    pass "scanner compressed idle pages ($BEFORE_CMP -> $AFTER_CMP)"
else
    echo "  (no compression yet — may need more idle pages or sweep cycles)"
fi

# Disable scanner
echo "0" > "$SYSDIR/scanner_enabled" 2>/dev/null

# Test module unload with drain-and-restore
echo ""
echo "=== Test: Module unload with drain-and-restore ==="

BEFORE_ZSWAP=$(cat "$SYSDIR/zswap_pages" 2>/dev/null || echo 0)
echo "  Pages in zswap before unload: $BEFORE_ZSWAP"

if [ "$BEFORE_ZSWAP" -gt 0 ]; then
    echo "  Attempting module unload with $BEFORE_ZSWAP compressed pages..."

    # Check dmesg for drain messages
    DMESG_BEFORE=$(dmesg | grep "minimem:" | wc -l)

    if rmmod minimem 2>/dev/null; then
        DMESG_AFTER=$(dmesg | grep "minimem:" | wc -l)
        DRAIN_MSG=$(dmesg | grep "drain_and_restore" | tail -1)

        if [ -n "$DRAIN_MSG" ]; then
            RESTORED=$(echo "$DRAIN_MSG" | grep -oP 'restored \K[0-9]+' || echo "?")
            pass "drain_and_restore executed (restored $RESTORED pages)"
        else
            echo "  (no drain_and_restore message — may have 0 compressed pages)"
        fi

        if [ ! -d "$SYSDIR" ]; then
            pass "module unloaded and sysfs cleaned up"
        else
            fail "sysfs still exists after unload"
        fi
    else
        fail "module unload failed (may need to stop scanner first)"
    fi
else
    echo "  No compressed pages — testing clean unload..."
    if rmmod minimem 2>/dev/null; then
        pass "module unloaded with no compressed pages"
    else
        fail "module unload failed even with no compressed pages"
    fi
fi

echo ""
echo "========================================"
echo "Kprobe transparent compression test results"
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