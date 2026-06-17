#!/bin/bash
# scripts/publish-all.sh — Publish MiniMem to all distribution channels
#
# Publishes to:
#   1. AUR (Arch Linux)
#   2. OBS (Fedora, Debian, Ubuntu, openSUSE)
#
# Usage: ./scripts/publish-all.sh [--aur-only] [--obs-only] [--skip-aur] [--skip-obs]

set -e

VERSION="0.7.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OBS_USER="${OBS_USER:-}"

SKIP_AUR=false
SKIP_OBS=false

for arg in "$@"; do
    case "$arg" in
        --aur-only) SKIP_OBS=true ;;
        --obs-only) SKIP_AUR=true ;;
        --skip-aur) SKIP_AUR=true ;;
        --skip-obs) SKIP_OBS=true ;;
    esac
done

echo "=== MiniMem Publish All ==="
echo "  Version: $VERSION"
echo ""

# Pre-flight checks
ERRORS=0

if [ "$SKIP_AUR" = false ] && ! command -v makepkg &>/dev/null; then
    echo "Warning: makepkg not found. AUR publish will fail on this machine."
    ERRORS=$((ERRORS + 1))
fi

if [ "$SKIP_OBS" = false ] && ! command -v osc &>/dev/null; then
    echo "Warning: osc not found. OBS publish will fail on this machine."
    echo "  Install: pip install osc"
    ERRORS=$((ERRORS + 1))
fi

if [ "$ERRORS" -gt 0 ]; then
    echo ""
    echo "Install missing tools, or use --skip-aur / --skip-obs to skip."
    echo "Continuing with available channels ..."
fi

# Verify source tarball
SOURCE_URL="https://github.com/marcel-b-roodt/MiniMem/archive/refs/tags/v${VERSION}.tar.gz"
if ! curl -fsI "$SOURCE_URL" >/dev/null 2>&1; then
    echo "Error: Git tag v$VERSION not found on GitHub."
    echo "Create and push the tag first:"
    echo "  git tag -s v$VERSION -m 'MiniMem v$VERSION'"
    echo "  git push origin v$VERSION"
    exit 1
fi

# 1. AUR
if [ "$SKIP_AUR" = false ]; then
    echo ""
    echo "=== Publishing to AUR ==="
    if [ -f "$PROJECT_DIR/scripts/publish-aur.sh" ]; then
        bash "$PROJECT_DIR/scripts/publish-aur.sh" --update
    else
        echo "AUR publish script not found. Skipping."
    fi
fi

# 2. OBS
if [ "$SKIP_OBS" = false ]; then
    echo ""
    echo "=== Publishing to OBS ==="
    if [ -f "$PROJECT_DIR/scripts/publish-obs.sh" ]; then
        bash "$PROJECT_DIR/scripts/publish-obs.sh" --update
    else
        echo "OBS publish script not found. Skipping."
    fi
fi

echo ""
echo "=== Publish Complete ==="
echo ""
echo "Users can install from:"
echo ""
echo "  Arch Linux:     yay -S minimem minimem-dkms"
echo "  Fedora:         dnf install libminimem minimem-dkms  (from OBS repo)"
echo "  Debian/Ubuntu:  apt install libminimem0 minimem-dkms  (from OBS repo)"
echo "  openSUSE:       zypper install libminimem minimem-dkms  (from OBS repo)"
echo "  Manual DKMS:    sudo ./scripts/dkms-install.sh"
echo ""
echo "Add OBS repo (one-time, per distro):"
echo "  Fedora:     See https://build.opensuse.org/project/show/home:${OBS_USER:-YOUR_OBS_USER}"
echo "  Debian:     See https://build.opensuse.org/project/show/home:${OBS_USER:-YOUR_OBS_USER}"
echo "  Ubuntu:     See https://build.opensuse.org/project/show/home:${OBS_USER:-YOUR_OBS_USER}"