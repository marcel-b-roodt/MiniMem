#!/bin/bash
# tests/test_dkms_packaging.sh — Verify DKMS packaging correctness
#
# Run without root. Checks file layout, script syntax, idempotency,
# and PKGBUILD correctness. Does NOT actually install (needs sudo).
#
# Usage: ./tests/test_dkms_packaging.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DKMS_DIR="$PROJECT_DIR/dkms"
PATCHES_DIR="$PROJECT_DIR/patches"
AUR_DIR="$PROJECT_DIR/packaging/aur"
SCRIPTS_DIR="$PROJECT_DIR/scripts"

PASS=0
FAIL=0

ok() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== DKMS Packaging Tests ==="
echo ""

echo "--- File Layout ---"
[ -f "$DKMS_DIR/dkms.conf" ] && ok "dkms.conf exists" || fail "dkms.conf missing"
[ -f "$DKMS_DIR/Makefile" ] && ok "DKMS Makefile exists" || fail "DKMS Makefile missing"
[ -f "$DKMS_DIR/install.sh" ] && ok "install.sh exists" || fail "install.sh missing"
[ -f "$DKMS_DIR/uninstall.sh" ] && ok "uninstall.sh exists" || fail "uninstall.sh missing"
[ -x "$DKMS_DIR/install.sh" ] && ok "install.sh executable" || fail "install.sh not executable"
[ -x "$DKMS_DIR/uninstall.sh" ] && ok "uninstall.sh executable" || fail "uninstall.sh not executable"
[ -f "$SCRIPTS_DIR/dkms-install.sh" ] && ok "dkms-install.sh exists" || fail "dkms-install.sh missing"
[ -f "$SCRIPTS_DIR/dkms-uninstall.sh" ] && ok "dkms-uninstall.sh exists" || fail "dkms-uninstall.sh missing"
[ -x "$SCRIPTS_DIR/dkms-install.sh" ] && ok "dkms-install.sh executable" || fail "dkms-install.sh not executable"
[ -x "$SCRIPTS_DIR/dkms-uninstall.sh" ] && ok "dkms-uninstall.sh executable" || fail "dkms-uninstall.sh not executable"

echo ""
echo "--- dkms.conf ---"
grep -q 'PACKAGE_NAME="minimem"' "$DKMS_DIR/dkms.conf" && ok "PACKAGE_NAME set" || fail "PACKAGE_NAME missing"
grep -q 'PACKAGE_VERSION="0.6.0"' "$DKMS_DIR/dkms.conf" && ok "PACKAGE_VERSION set" || fail "PACKAGE_VERSION missing"
grep -q 'BUILT_MODULE_NAME="minimem"' "$DKMS_DIR/dkms.conf" && ok "BUILT_MODULE_NAME set" || fail "BUILT_MODULE_NAME missing"
grep -q 'AUTOINSTALL="yes"' "$DKMS_DIR/dkms.conf" && ok "AUTOINSTALL set" || fail "AUTOINSTALL missing"
grep -q 'POST_INSTALL' "$DKMS_DIR/dkms.conf" && ok "POST_INSTALL defined" || fail "POST_INSTALL missing"
grep -q 'PRE_REMOVE' "$DKMS_DIR/dkms.conf" && ok "PRE_REMOVE defined" || fail "PRE_REMOVE missing"
grep -q 'PATCHES' "$DKMS_DIR/dkms.conf" && ok "PATCHES defined" || fail "PATCHES missing"

echo ""
echo "--- DKMS Makefile ---"
grep -q 'obj-m += minimem.o' "$DKMS_DIR/Makefile" && ok "obj-m set" || fail "obj-m missing"
grep -q 'minimem-y' "$DKMS_DIR/Makefile" && ok "module objects defined" || fail "module objects missing"
grep -q 'KERNELDIR' "$DKMS_DIR/Makefile" && ok "KERNELDIR variable" || fail "KERNELDIR variable missing"
grep -q 'MINIMEM_KERNEL' "$DKMS_DIR/Makefile" && ok "MINIMEM_KERNEL flag set" || fail "MINIMEM_KERNEL flag missing"

echo ""
echo "--- Patches ---"
[ -f "$PATCHES_DIR/series" ] && ok "patches/series exists" || fail "patches/series missing"
patch_count=$(ls -1 "$PATCHES_DIR"/minimem-*.patch 2>/dev/null | wc -l)
[ "$patch_count" -ge 2 ] && ok "$patch_count patch files found" || fail "expected 2+ patches, found $patch_count"

while IFS= read -r patchname; do
    [ -f "$PATCHES_DIR/$patchname" ] && ok "patches/series references $patchname" || fail "patches/series references missing $patchname"
done < "$PATCHES_DIR/series"

echo ""
echo "--- Script Syntax ---"
bash -n "$DKMS_DIR/install.sh" && ok "install.sh syntax OK" || fail "install.sh syntax error"
bash -n "$DKMS_DIR/uninstall.sh" && ok "uninstall.sh syntax OK" || fail "uninstall.sh syntax error"
bash -n "$SCRIPTS_DIR/dkms-install.sh" && ok "dkms-install.sh syntax OK" || fail "dkms-install.sh syntax error"
bash -n "$SCRIPTS_DIR/dkms-uninstall.sh" && ok "dkms-uninstall.sh syntax OK" || fail "dkms-uninstall.sh syntax error"

echo ""
echo "--- Reboot Messaging ---"
grep -qi "reboot" "$DKMS_DIR/install.sh" && ok "install.sh mentions reboot" || fail "install.sh missing reboot warning"
grep -qi "reboot" "$DKMS_DIR/uninstall.sh" && ok "uninstall.sh mentions reboot" || fail "uninstall.sh missing reboot warning"
grep -qi "rebuild" "$DKMS_DIR/install.sh" && ok "install.sh mentions rebuild" || fail "install.sh missing rebuild instruction"
grep -qi "rebuild" "$SCRIPTS_DIR/dkms-install.sh" && ok "dkms-install.sh mentions rebuild" || fail "dkms-install.sh missing rebuild instruction"

echo ""
echo "--- Idempotency ---"
grep -q 'include/linux/minimem.h' "$DKMS_DIR/install.sh" && ok "install.sh checks for existing patches" || fail "install.sh doesn't check existing patches"
grep -q -- '--dry-run' "$DKMS_DIR/install.sh" && ok "install.sh uses dry-run" || fail "install.sh missing dry-run"
grep -q -- '--dry-run' "$DKMS_DIR/uninstall.sh" && ok "uninstall.sh uses dry-run" || fail "uninstall.sh missing dry-run"

echo ""
echo "--- Module Unload on Remove ---"
grep -qi 'modprobe -r\|rmmod' "$DKMS_DIR/uninstall.sh" && ok "uninstall.sh unloads module" || fail "uninstall.sh doesn't unload module"
grep -qi 'modprobe -r\|rmmod' "$SCRIPTS_DIR/dkms-uninstall.sh" && ok "dkms-uninstall.sh unloads module" || fail "dkms-uninstall.sh doesn't unload module"

echo ""
echo "--- AUR PKGBUILDs ---"
[ -f "$AUR_DIR/minimem/PKGBUILD" ] && ok "minimem PKGBUILD exists" || fail "minimem PKGBUILD missing"
[ -f "$AUR_DIR/minimem-dkms/PKGBUILD" ] && ok "minimem-dkms PKGBUILD exists" || fail "minimem-dkms PKGBUILD missing"
[ -f "$AUR_DIR/minimem-dkms/minimem-dkms.install" ] && ok "minimem-dkms.install exists" || fail "minimem-dkms.install missing"

grep -q 'pkgname=minimem$' "$AUR_DIR/minimem/PKGBUILD" && ok "lib PKGBUILD pkgname" || fail "lib PKGBUILD wrong pkgname"
grep -q 'meson' "$AUR_DIR/minimem/PKGBUILD" && ok "lib PKGBUILD uses meson" || fail "lib PKGBUILD missing meson"
grep -q 'pkgname=minimem-dkms' "$AUR_DIR/minimem-dkms/PKGBUILD" && ok "dkms PKGBUILD pkgname" || fail "dkms PKGBUILD wrong pkgname"
grep -q 'dkms' "$AUR_DIR/minimem-dkms/PKGBUILD" && ok "dkms PKGBUILD depends on dkms" || fail "dkms PKGBUILD missing dkms dep"

echo ""
echo "--- Kernel Patch Detection (runtime) ---"
grep -q 'kernel_patches_detected' "$PROJECT_DIR/src/kernel/minimem_hook.c" && ok "hook.c has kernel_patches_detected" || fail "hook.c missing kernel_patches_detected"
grep -q 'minimem_register_fault_handler' "$PROJECT_DIR/src/kernel/minimem_hook.c" && ok "hook.c resolves register symbol" || fail "hook.c missing register symbol resolution"
grep -q 'minimem_vm_fault_handler' "$PROJECT_DIR/src/kernel/minimem_hook.c" && ok "hook.c has vm_fault_handler" || fail "hook.c missing vm_fault_handler"
grep -q 'kernel_patches' "$PROJECT_DIR/src/kernel/minimem_main.c" && ok "sysfs kernel_patches attribute" || fail "sysfs kernel_patches attribute missing"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0