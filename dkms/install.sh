#!/bin/bash
# MiniMem DKMS post-install script
# Called by DKMS after the module is built and installed.
# Applies kernel patches if they haven't been applied yet.
#
# IMPORTANT: Kernel patches modify kernel source but do NOT take effect
# until the kernel is rebuilt and rebooted into. The module works without
# patches (kprobe-only mode), but the scanner sweep pass requires a
# patched kernel.

set -e

KVER="${1:-$(uname -r)}"
KERNELDIR="/usr/lib/modules/$KVER/build"
PATCHDIR="/usr/src/minimem-0.8.0/patches"

echo "minimem-dkms: post-install for kernel $KVER"

if [ ! -d "$KERNELDIR" ]; then
    echo "minimem-dkms: kernel source directory not found: $KERNELDIR"
    echo "minimem-dkms: kernel patches skipped (module built without patch support)"
    echo "minimem-dkms: module will run in kprobe-only mode (scanner sweep disabled)"
    exit 0
fi

if [ -f "$KERNELDIR/include/linux/minimem.h" ]; then
    echo "minimem-dkms: kernel patches already applied (include/linux/minimem.h exists)"
    echo "minimem-dkms: if you have not yet rebuilt and rebooted, do so now."
    exit 0
fi

echo "minimem-dkms: applying kernel patches to $KERNELDIR ..."

if [ ! -d "$PATCHDIR" ]; then
    echo "minimem-dkms: patch directory not found: $PATCHDIR"
    exit 1
fi

KMAJ=$(echo "$KVER" | cut -d. -f1)
KMIN=$(echo "$KVER" | cut -d. -f2)

APPLIED=0
FAILED=0

for patchfile in "$PATCHDIR"/minimem-"${KMAJ}"."${KMIN}"-*.patch "$PATCHDIR"/minimem-*.patch; do
    [ -f "$patchfile" ] || continue
    case "$patchfile" in
        *"/minimem-${KMAJ}.${KMIN}-"*) ;;
        *"/minimem-"*)
            if ls "$PATCHDIR"/minimem-${KMAJ}.${KMIN}-*.patch >/dev/null 2>&1; then
                continue
            fi
            ;;
    esac
    patchname=$(basename "$patchfile")
    echo "minimem-dkms: applying $patchname ..."
    if patch -d "$KERNELDIR" -p1 --dry-run < "$patchfile" >/dev/null 2>&1; then
        patch -d "$KERNELDIR" -p1 < "$patchfile"
        APPLIED=$((APPLIED + 1))
        echo "minimem-dkms: $patchname applied successfully"
    else
        FAILED=$((FAILED + 1))
        echo "minimem-dkms: WARNING: $patchname dry-run failed (already applied or kernel version mismatch)"
    fi
done

if [ "$APPLIED" -gt 0 ]; then
    echo ""
    echo "=========================================================="
    echo "  KERNEL PATCHES APPLIED — REBUILD + REBOOT REQUIRED"
    echo "=========================================================="
    echo ""
    echo "  $APPLIED patch(es) applied to $KERNELDIR"
    echo ""
    echo "  The patches will NOT take effect until you rebuild the"
    echo "  kernel and reboot. Steps:"
    echo ""
    echo "    1. cd $KERNELDIR"
    echo "    2. make -j\$(nproc)                # rebuild kernel"
    echo "    3. sudo make modules_install install  # install new kernel"
    echo "    4. sudo update-grub                  # update bootloader"
    echo "    5. sudo reboot                      # boot into patched kernel"
    echo ""
    echo "  After reboot, the scanner sweep pass will be available."
    echo "  Check: cat /sys/kernel/minimem/kernel_patches  (should be 1)"
    echo ""
    echo "  WITHOUT REBOOT: the module still works in kprobe-only mode."
    echo "  The scanner sweep pass is disabled until reboot."
    echo "=========================================================="
fi

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo "minimem-dkms: $FAILED patch(es) could not be applied."
    echo "minimem-dkms: module will run in kprobe-only mode (scanner sweep disabled)."
    echo "minimem-dkms: check /sys/kernel/minimem/kernel_patches after loading module."
fi

depmod -a "$KVER" 2>/dev/null || true

echo "minimem-dkms: post-install complete"
exit 0