#!/bin/bash
# scripts/dkms-uninstall.sh — Remove MiniMem DKMS module
#
# Reverses kernel patches, unloads module, removes DKMS registration.
#
# Usage: sudo ./scripts/dkms-uninstall.sh

set -e

VERSION="0.6.0"
DKMS_NAME="minimem"
DKMS_DIR="/usr/src/${DKMS_NAME}-${VERSION}"
KVER="$(uname -r)"

echo "=== MiniMem DKMS Uninstall ==="

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root (sudo)"
    exit 1
fi

if [ -d "$DKMS_DIR" ] && [ -f "$DKMS_DIR/uninstall.sh" ]; then
    echo "Reversing kernel patches (if applied) ..."
    bash "$DKMS_DIR/uninstall.sh" "$KVER" || true
else
    echo "DKMS source directory not found — skipping patch reversal"
fi

if ! dkms status "$DKMS_NAME/$VERSION" 2>/dev/null | grep -q "$DKMS_NAME"; then
    echo "MiniMem DKMS module is not registered."
else
    if lsmod 2>/dev/null | grep -q "^minimem "; then
        echo "Unloading module ..."
        modprobe -r minimem 2>/dev/null || {
            echo "WARNING: could not unload module (may be in use)"
            echo "Unload manually: sudo rmmod minimem"
        }
    fi

    echo "Removing from DKMS ..."
    dkms remove "$DKMS_NAME/$VERSION" --all
fi

if [ -d "$DKMS_DIR" ]; then
    echo "Removing source directory ..."
    rm -rf "$DKMS_DIR"
fi

echo ""
echo "=== MiniMem DKMS Uninstall Complete ==="
echo ""
echo "If kernel patches were reversed, you must rebuild and reboot"
echo "your kernel for the reversal to take effect."