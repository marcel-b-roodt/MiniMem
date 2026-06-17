#!/bin/bash
# scripts/publish-debian.sh — Build and publish MiniMem Debian packages
#
# Prerequisites:
#   - devscripts, debhelper, meson, libzstd-dev installed
#   - git tag v$VERSION exists on GitHub
#
# Usage: ./scripts/publish-debian.sh [--build-only]
#
# What it does:
#   1. Downloads source tarball from GitHub
#   2. Builds libminimem-dev, libminimem0, minimem-dkms .deb packages
#   3. Optionally uploads to a PPA or repo (--upload)
#
# Users install via:
#   sudo dpkg -i libminimem0_*.deb libminimem-dev_*.deb
#   sudo dpkg -i minimem-dkms_*.deb

set -e

VERSION="0.7.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="/tmp/minimem-debian-build"
SOURCE_URL="https://github.com/marcel-b-roodt/MiniMem/archive/refs/tags/v${VERSION}.tar.gz"
BUILD_ONLY=false
UPLOAD=false

for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=true ;;
        --upload) UPLOAD=true ;;
    esac
done

echo "=== MiniMem Debian Package Build ==="
echo "  Version: $VERSION"
echo ""

if ! command -v dpkg-buildpackage &>/dev/null; then
    echo "Error: dpkg-buildpackage not found. Run on Debian/Ubuntu."
    echo "Install: sudo apt install devscripts debhelper"
    exit 1
fi

# Download source
mkdir -p "$BUILD_DIR"
TARBALL="$BUILD_DIR/minimem_$VERSION.orig.tar.gz"

if [ ! -f "$TARBALL" ]; then
    echo "Downloading source tarball ..."
    curl -fL "$SOURCE_URL" -o "$TARBALL"
fi

# Extract
echo "Extracting source ..."
rm -rf "$BUILD_DIR/MiniMem-$VERSION"
tar -xzf "$TARBALL" -C "$BUILD_DIR"

SOURCE_DIR="$BUILD_DIR/MiniMem-$VERSION"

# Copy debian packaging
echo "Adding debian/ directory ..."
cp -r "$PROJECT_DIR/packaging/debian" "$SOURCE_DIR/debian"

# Build packages
echo "Building Debian packages ..."
(cd "$SOURCE_DIR" && \
    dpkg-buildpackage -us -uc -b)

echo ""
echo "Debian packages built:"
ls -la "$BUILD_DIR"/*.deb 2>/dev/null

if [ "$BUILD_ONLY" = true ]; then
    echo ""
    echo "Build-only mode. Packages are in: $BUILD_DIR/"
    echo "Install with: sudo dpkg -i $BUILD_DIR/*.deb"
    exit 0
fi

if [ "$UPLOAD" = true ]; then
    echo ""
    echo "Upload not yet implemented."
    echo "To publish to a PPA or repo, use dput or aptly."
    echo "See: https://wiki.debian.org/HowToSetupADebianRepository"
    exit 0
fi

echo ""
echo "Packages ready for local install:"
echo "  sudo dpkg -i $BUILD_DIR/libminimem0_*.deb"
echo "  sudo dpkg -i $BUILD_DIR/libminimem-dev_*.deb"
echo "  sudo dpkg -i $BUILD_DIR/minimem-dkms_*.deb"