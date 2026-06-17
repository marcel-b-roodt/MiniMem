#!/bin/bash
# scripts/publish-fedora.sh — Build and publish MiniMem RPM packages
#
# Prerequisites:
#   - rpm-build, meson, gcc, libzstd-devel, dkms installed
#   - git tag v$VERSION exists on GitHub
#
# Usage: ./scripts/publish-fedora.sh [--build-only]
#
# What it does:
#   1. Downloads source tarball from GitHub
#   2. Builds libminimem-devel and minimem-dkms RPM packages
#   3. Optionally creates a COPR repo (--copr)
#
# Users install via:
#   sudo dnf install libminimem-devel minimem-dkms

set -e

VERSION="0.7.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="/tmp/minimem-fedora-build"
SOURCE_URL="https://github.com/marcel-b-roodt/MiniMem/archive/refs/tags/v${VERSION}.tar.gz"
BUILD_ONLY=false

for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=true ;;
    esac
done

echo "=== MiniMem Fedora/RPM Package Build ==="
echo "  Version: $VERSION"
echo ""

if ! command -v rpmbuild &>/dev/null; then
    echo "Error: rpmbuild not found. Run on Fedora/RHEL."
    echo "Install: sudo dnf install rpm-build"
    exit 1
fi

# Setup rpmbuild tree
RPMBUILD_DIR="$BUILD_DIR/rpmbuild"
mkdir -p "$RPMBUILD_DIR"/{SOURCES,SPECS,BUILD,RPMS,SRPMS}

# Download source
TARBALL="$RPMBUILD_DIR/SOURCES/minimem-$VERSION.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "Downloading source tarball ..."
    curl -fL "$SOURCE_URL" -o "$TARBALL"
fi

# Copy spec
echo "Copying spec file ..."
cp "$PROJECT_DIR/packaging/fedora/minimem.spec" "$RPMBUILD_DIR/SPECS/"

# Build RPMs
echo "Building RPM packages ..."
rpmbuild -bb "$RPMBUILD_DIR/SPECS/minimem.spec" \
    --define "_topdir $RPMBUILD_DIR" \
    --define "version $VERSION"

echo ""
echo "RPM packages built:"
find "$RPMBUILD_DIR/RPMS" -name '*.rpm' -exec ls -la {} \;

if [ "$BUILD_ONLY" = true ]; then
    echo ""
    echo "Build-only mode. Packages are in: $RPMBUILD_DIR/RPMS/"
    echo "Install with: sudo dnf install $RPMBUILD_DIR/RPMS/*/*.rpm"
    exit 0
fi

echo ""
echo "To publish to COPR:"
echo "  1. Create a COPR project: https://copr.fedorainfracloud.org/"
echo "  2. Upload SRPM: copr-cli build minimem $RPMBUILD_DIR/SRPMS/*.src.rpm"
echo ""
echo "For local install:"
echo "  sudo dnf install $RPMBUILD_DIR/RPMS/*/*.rpm"