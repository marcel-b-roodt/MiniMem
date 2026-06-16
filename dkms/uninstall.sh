#!/bin/bash
# MiniMem DKMS pre-remove script
# Called by DKMS before the module is removed.
# Reverses kernel patches if they were applied.
#
# IMPORTANT: Patch reversal modifies kernel source but does NOT take
# effect until the kernel is rebuilt and rebooted into.

set -e

KVER="${1:-$(uname -r)}"
KERNELDIR="/usr/lib/modules/$KVER/build"
PATCHDIR="/usr/src/minimem-0.6.0/patches"

echo "minimem-dkms: pre-remove for kernel $KVER"

if lsmod 2>/dev/null | grep -q "^minimem "; then
    echo "minimem-dkms: module is loaded, attempting to unload ..."
    modprobe -r minimem 2>/dev/null || {
        echo "minimem-dkms: WARNING: could not unload module. It may be in use."
        echo "minimem-dkms: Unload manually: sudo rmmod minimem"
    }
fi

if [ ! -d "$KERNELDIR" ]; then
    echo "minimem-dkms: kernel source directory not found, skipping patch removal"
    exit 0
fi

if [ ! -f "$KERNELDIR/include/linux/minimem.h" ]; then
    echo "minimem-dkms: kernel patches not applied (include/linux/minimem.h not found), skipping"
    exit 0
fi

echo "minimem-dkms: reversing kernel patches in $KERNELDIR ..."

PATCHES=$(ls -1r "$PATCHDIR"/minimem-*.patch 2>/dev/null)

if [ -z "$PATCHES" ]; then
    echo "minimem-dkms: no patches found in $PATCHDIR"
    exit 0
fi

REVERSED=0
FAILED=0

for patchfile in $PATCHES; do
    patchname=$(basename "$patchfile")
    echo "minimem-dkms: reversing $patchname ..."
    if patch -d "$KERNELDIR" -p1 -R --dry-run < "$patchfile" >/dev/null 2>&1; then
        patch -d "$KERNELDIR" -p1 -R < "$patchfile"
        REVERSED=$((REVERSED + 1))
        echo "minimem-dkms: $patchname reversed successfully"
    else
        FAILED=$((FAILED + 1))
        echo "minimem-dkms: WARNING: $patchname reverse dry-run failed"
    fi
done

if [ "$REVERSED" -gt 0 ]; then
    echo ""
    echo "=========================================================="
    echo "  KERNEL PATCHES REVERSED — REBUILD + REBOOT REQUIRED"
    echo "=========================================================="
    echo ""
    echo "  $REVERSED patch(es) reversed in $KERNELDIR"
    echo ""
    echo "  The reversal will NOT take effect until you rebuild the"
    echo "  kernel and reboot. MiniMem markers in PTEs from any"
    echo "  currently compressed pages will cause SIGBUS after reboot"
    echo "  unless the module is re-installed."
    echo ""
    echo "  If you have compressed pages, unload the module first:"
    echo "    sudo rmmod minimem"
    echo ""
    echo "  Then rebuild and reboot to finalize patch removal."
    echo "=========================================================="
fi

if [ "$FAILED" -gt 0 ]; then
    echo "minimem-dkms: $FAILED patch(es) could not be reversed cleanly."
    echo "minimem-dkms: you may need to manually clean up kernel source."
fi

depmod -a "$KVER" 2>/dev/null || true

echo "minimem-dkms: pre-remove complete"
exit 0