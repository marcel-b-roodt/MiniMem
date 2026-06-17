#!/bin/bash
# scripts/publish-aur.sh — Publish MiniMem to AUR (Arch Linux)
#
# AUR packages: minimem (library) and minimem-dkms (kernel module)
#
# First-time setup (manual, one-off):
#   1. Create an AUR account at https://aur.archlinux.org/register
#   2. Upload your SSH key at https://aur.archlinux.org/account/
#   3. Clone the AUR packages (initial):
#        AUR_SSH_USER=you ./scripts/publish-aur.sh --init
#
# After setup:
#   AUR_SSH_USER=you ./scripts/publish-aur.sh --update
#   ./scripts/publish-aur.sh --check
#   ./scripts/publish-aur.sh --setup-guide

set -euo pipefail

VERSION="0.7.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
AUR_SSH_USER="${AUR_SSH_USER:-aur}"
AUR_SSH="${AUR_SSH_USER}@aur.archlinux.org"
PACKAGING_DIR="$PROJECT_DIR/packaging/aur"
WORK_DIR="/tmp/minimem-aur-work"

ACTION="${1:---setup-guide}"

case "$ACTION" in
    --setup-guide)
        echo "=== MiniMem AUR Setup Guide ==="
        echo ""
        echo "1. Create an AUR account:"
        echo "   https://aur.archlinux.org/register"
        echo ""
        echo "2. Add your SSH key to your AUR account:"
        echo "   https://aur.archlinux.org/account/"
        echo ""
        echo "3. Initialize AUR repos:"
        echo "   AUR_SSH_USER=you ./scripts/publish-aur.sh --init"
        echo ""
        echo "4. After init, update packages:"
        echo "   AUR_SSH_USER=you ./scripts/publish-aur.sh --update"
        echo ""
        echo "AUR packages:"
        echo "  minimem        — userspace compression library"
        echo "  minimem-dkms   — kernel module (DKMS, auto-rebuilds on kernel update)"
        ;;

    --init)
        echo "=== Initializing AUR package repos ==="
        mkdir -p "$WORK_DIR"

        if [ ! -d "$WORK_DIR/minimem" ]; then
            echo "Cloning minimem ..."
            git clone "ssh://$AUR_SSH/minimem.git" "$WORK_DIR/minimem"
        fi

        if [ ! -d "$WORK_DIR/minimem-dkms" ]; then
            echo "Cloning minimem-dkms ..."
            git clone "ssh://$AUR_SSH/minimem-dkms.git" "$WORK_DIR/minimem-dkms"
        fi

        echo ""
        echo "AUR repos cloned to $WORK_DIR/"
        echo "Run 'AUR_SSH_USER=you ./scripts/publish-aur.sh --update' to push PKGBUILDs."
        ;;

    --update)
        if [ ! -d "$WORK_DIR/minimem" ] || [ ! -d "$WORK_DIR/minimem-dkms" ]; then
            echo "Error: AUR repos not initialized. Run './scripts/publish-aur.sh --init' first."
            exit 1
        fi

        echo "=== Updating AUR packages to v$VERSION ==="

        for pkg in minimem minimem-dkms; do
            echo "--- $pkg ---"
            PKG_DIR="$WORK_DIR/$pkg"

            cp "$PACKAGING_DIR/$pkg/PKGBUILD" "$PKG_DIR/PKGBUILD"

            if [ "$pkg" = "minimem-dkms" ]; then
                cp "$PACKAGING_DIR/$pkg/minimem-dkms.install" "$PKG_DIR/"
            fi

            (cd "$PKG_DIR" && \
                makepkg --printsrcinfo > .SRCINFO && \
                git add PKGBUILD .SRCINFO && \
                [ -f minimem-dkms.install ] && git add minimem-dkms.install || true && \
                git commit -m "Update to v$VERSION" && \
                git push)
        done

        echo ""
        echo "AUR packages updated."
        echo "  https://aur.archlinux.org/packages/minimem"
        echo "  https://aur.archlinux.org/packages/minimem-dkms"
        ;;

    --check)
        echo "=== Verifying AUR packaging ==="
        for pkg in minimem minimem-dkms; do
            PKG_DIR="$PACKAGING_DIR/$pkg"
            echo "--- $pkg ---"

            if [ ! -f "$PKG_DIR/PKGBUILD" ]; then
                echo "  ERROR: PKGBUILD missing"
                continue
            fi

            tmpdir=$(mktemp -d)
            cp "$PKG_DIR/PKGBUILD" "$tmpdir/"

            if [ "$pkg" = "minimem-dkms" ] && [ -f "$PKG_DIR/minimem-dkms.install" ]; then
                cp "$PKG_DIR/minimem-dkms.install" "$tmpdir/"
            fi

            (cd "$tmpdir" && makepkg --printsrcinfo > .SRCINFO 2>/dev/null) || {
                echo "  WARNING: could not generate .SRCINFO (missing deps is OK on non-Arch)"
            }

            if [ -f "$tmpdir/.SRCINFO" ] && [ -f "$PKG_DIR/.SRCINFO" ]; then
                if diff -q "$tmpdir/.SRCINFO" "$PKG_DIR/.SRCINFO" >/dev/null 2>&1; then
                    echo "  .SRCINFO matches PKGBUILD"
                else
                    echo "  WARNING: .SRCINFO is stale, run --update to regenerate"
                fi
            fi

            rm -rf "$tmpdir"
        done
        ;;

    *)
        echo "Usage: $0 [--setup-guide|--init|--update|--check]"
        echo ""
        echo "  --setup-guide  : Print first-time AUR setup instructions"
        echo "  --init         : Clone AUR repos (first time only)"
        echo "  --update       : Push updated PKGBUILDs to AUR"
        echo "  --check        : Verify .SRCINFO matches PKGBUILD"
        echo ""
        echo "Environment:"
        echo "  AUR_SSH_USER : SSH username for AUR (default: aur)"
        exit 1
        ;;
esac