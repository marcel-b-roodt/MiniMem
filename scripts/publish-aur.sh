#!/bin/bash
# scripts/publish-aur.sh — Publish MiniMem packages to the AUR
#
# Prerequisites:
#   - git tag v$VERSION exists on GitHub (for source tarball URL)
#   - ssh key configured for aur.archlinux.org
#   - makepkg installed (Arch Linux)
#
# Usage: ./scripts/publish-aur.sh [minimem|minimem-dkms|both]
#
# What it does:
#   1. Clones the AUR repos (or pulls if they exist)
#   2. Generates up-to-date PKGBUILD and .SRCINFO
#   3. Runs makepkg --verifysource to check the source tarball resolves
#   4. Commits and pushes to AUR
#
# After publishing, users can install via:
#   yay -S minimem          # library
#   yay -S minimem-dkms     # kernel module

set -e

VERSION="0.6.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
AUR_BASE="${AUR_BASE:-$HOME/aur}"
PACKAGING_DIR="$PROJECT_DIR/packaging/aur"

# Which package(s) to publish
PUBLISH_MINIMEM=true
PUBLISH_DKMS=true

if [ "$1" = "minimem" ]; then
    PUBLISH_DKMS=false
elif [ "$1" = "minimem-dkms" ]; then
    PUBLISH_MINIMEM=false
elif [ "$1" != "" ] && [ "$1" != "both" ]; then
    echo "Usage: $0 [minimem|minimem-dkms|both]"
    exit 1
fi

echo "=== MiniMem AUR Publish ==="
echo "  Version: $VERSION"
echo "  AUR base: $AUR_BASE"
echo ""

# Verify prerequisites
if ! command -v makepkg &>/dev/null; then
    echo "Error: makepkg not found. This script must run on Arch Linux."
    exit 1
fi

if ! ssh -T aur@aur.archlinux.org 2>&1 | grep -q "interactive"; then
    echo "Warning: SSH key for aur.archlinux.org may not be configured."
    echo "Test with: ssh -T aur@aur.archlinux.org"
    echo "Continuing anyway..."
fi

# Verify the GitHub tag exists (source tarball must resolve)
SOURCE_URL="https://github.com/marcel-b-roodt/MiniMem/archive/refs/tags/v${VERSION}.tar.gz"
echo "Verifying source tarball at $SOURCE_URL ..."
if curl -fsI "$SOURCE_URL" >/dev/null 2>&1; then
    echo "  Source tarball resolves OK"
else
    echo "Error: Source tarball not found at $SOURCE_URL"
    echo "Create and push the tag first: git tag -s v$VERSION && git push origin v$VERSION"
    exit 1
fi

mkdir -p "$AUR_BASE"

# ---- Helper: publish one AUR package ----
publish_aur_package() {
    local pkgname="$1"
    local aur_dir="$AUR_BASE/$pkgname"
    local src_dir="$PACKAGING_DIR/$pkgname"

    echo ""
    echo "--- Publishing $pkgname ---"

    # Clone or update AUR repo
    if [ -d "$aur_dir" ]; then
        echo "Updating existing AUR clone ..."
        git -C "$aur_dir" pull
    else
        echo "Cloning AUR repo for $pkgname ..."
        git clone "aur@aur.archlinux.org:$pkgname.git" "$aur_dir"
    fi

    # Copy PKGBUILD and supporting files
    echo "Copying PKGBUILD ..."
    cp "$src_dir/PKGBUILD" "$aur_dir/PKGBUILD"

    # Copy .install file if present
    if [ -f "$src_dir/$pkgname.install" ]; then
        cp "$src_dir/$pkgname.install" "$aur_dir/$pkgname.install"
    fi

    # Generate .SRCINFO
    echo "Generating .SRCINFO ..."
    (cd "$aur_dir" && makepkg --printsrcinfo > .SRCINFO)

    # Verify source
    echo "Verifying source ..."
    (cd "$aur_dir" && makepkg --verifysource 2>&1) || {
        echo "Warning: source verification failed. Check PKGBUILD source URL."
        echo "Continuing — you can fix before pushing."
    }

    # Show diff
    echo ""
    echo "Changes to be committed:"
    (cd "$aur_dir" && git diff --stat HEAD 2>/dev/null || true)
    (cd "$aur_dir" && git diff --stat --cached HEAD 2>/dev/null || true)
    (cd "$aur_dir" && git status --short 2>/dev/null || true)

    # Commit
    (cd "$aur_dir" && \
        git add PKGBUILD .SRCINFO ${pkgname}.install 2>/dev/null || true && \
        git commit -m "Update to $VERSION" && \
        git push)

    echo "$pkgname published to AUR"
}

# ---- Publish ----

if [ "$PUBLISH_MINIMEM" = true ]; then
    publish_aur_package "minimem"
fi

if [ "$PUBLISH_DKMS" = true ]; then
    publish_aur_package "minimem-dkms"
fi

echo ""
echo "=== AUR Publish Complete ==="
echo ""
echo "Users can now install:"
echo "  yay -S minimem          # userspace library"
echo "  yay -S minimem-dkms     # kernel module (DKMS)"
echo ""
echo "Verify at:"
echo "  https://aur.archlinux.org/packages/minimem"
echo "  https://aur.archlinux.org/packages/minimem-dkms"