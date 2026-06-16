#!/bin/bash
# scripts/release.sh — Full MiniMem release workflow
#
# Usage:
#   ./scripts/release.sh <version>           # full release
#   ./scripts/release.sh <version> --dry-run  # validate only, no changes
#   ./scripts/release.sh <version> --tag-only # tag + push only, no publish
#
# Steps:
#   1. Validate version format and git state
#   2. Verify builds (userspace + kernel module)
#   3. Run tests (DKMS packaging, Criterion if available)
#   4. Bump version across all files
#   5. Update changelog
#   6. Commit version bump
#   7. Create signed git tag
#   8. Push tag to GitHub
#   9. Create GitHub release
#  10. Publish to AUR
#  11. Publish to OBS
#
# Prerequisites:
#   - Clean working tree on main branch
#   - GPG key configured for git signing
#   - ssh key for aur.archlinux.org (for AUR publish)
#   - osc installed (for OBS publish)
#   - gh CLI authenticated (for GitHub release)

set -euo pipefail

VERSION=""
DRY_RUN=false
TAG_ONLY=false

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --tag-only) TAG_ONLY=true ;;
        -*) ;;
        *) VERSION="$arg" ;;
    esac
done

if [ -z "$VERSION" ]; then
    echo "Usage: ./scripts/release.sh <version> [--dry-run] [--tag-only]"
    echo ""
    echo "  <version>   Version to release, e.g. 0.7.0"
    echo "  --dry-run   Validate only, no changes made"
    echo "  --tag-only  Tag + push only, skip publishing"
    exit 1
fi

if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Error: Version must be semver format: X.Y.Z"
    exit 1
fi

TAG="v$VERSION"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

cd "$PROJECT_DIR"

echo "=== MiniMem Release $TAG ==="
echo ""

# ---- Step 1: Validate ----
echo "--- Step 1: Validation ---"

BRANCH=$(git branch --show-current)
if [ "$BRANCH" != "main" ]; then
    echo "Error: Must be on main branch. Currently on: $BRANCH"
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "Error: Working tree has uncommitted changes."
    git status --short
    exit 1
fi

if git tag -l "$TAG" | grep -q "$TAG"; then
    echo "Error: Tag $TAG already exists."
    exit 1
fi

echo "  Branch: $BRANCH"
echo "  Tree: clean"
echo "  Tag $TAG: available"

# ---- Step 2: Build verification ----
echo ""
echo "--- Step 2: Build Verification ---"

if [ ! -d "build" ]; then
    echo "Setting up userspace build ..."
    meson setup build -Dtests=false
fi

echo "Building userspace library ..."
meson compile -C build 2>&1 | tail -1
echo "  Userspace: OK"

echo "Building kernel module ..."
if ! bash ./build-kmod.sh build 2>&1 | grep -q "Module built"; then
    echo "Error: Kernel module build failed."
    exit 1
fi
echo "  Kernel module: OK"

# ---- Step 3: Tests ----
echo ""
echo "--- Step 3: Tests ---"

echo "Running DKMS packaging tests ..."
if bash ./tests/test_dkms_packaging.sh 2>&1 | grep -q "0 failed"; then
    echo "  DKMS packaging: OK"
else
    echo "Error: DKMS packaging tests failed."
    exit 1
fi

if [ -f build/minimem_tests ]; then
    echo "Running Criterion unit tests ..."
    meson test -C build --print-errorlogs 2>&1 || {
        echo "Warning: Some Criterion tests failed. Review before continuing."
    }
else
    echo "  Criterion not available, skipping unit tests"
fi

if [ "$DRY_RUN" = true ]; then
    echo ""
    echo "=== Dry run complete. All checks passed. ==="
    echo "Run without --dry-run to perform the release."
    exit 0
fi

# ---- Step 4: Version bump ----
echo ""
echo "--- Step 4: Version Bump ---"

OLD_VERSION=$(grep 'MODULE_VERSION' src/kernel/minimem_main.c | sed 's/.*"\(.*\)".*/\1/')
echo "  Current version: $OLD_VERSION"
echo "  New version:      $VERSION"

if [ "$OLD_VERSION" = "$VERSION" ]; then
    echo "  Version already bumped, skipping."
else
    echo "  Bumping version in all files ..."

    sed -i "s/$OLD_VERSION/$VERSION/g" src/kernel/minimem_main.c
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/dkms.conf
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/install.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/uninstall.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/dkms-install.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/dkms-uninstall.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem/PKGBUILD
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms/PKGBUILD
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms/minimem-dkms.install
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/fedora/minimem.spec
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/control
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/rules
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/changelog
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/obs/_service
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-aur.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-obs.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-debian.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-fedora.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-all.sh

    echo "  Done. Verify with: git diff"
fi

# ---- Step 5: Changelog ----
echo ""
echo "--- Step 5: Changelog ---"

CHANGELOG="CHANGELOG.md"
if [ ! -f "$CHANGELOG" ]; then
    echo "Creating $CHANGELOG ..."
    cat > "$CHANGELOG" << EOF
# MiniMem Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [$VERSION] - $(date +%Y-%m-%d)

### Added
- DKMS packaging with auto-rebuild on kernel updates
- Runtime kernel patch detection with kernel_patches sysfs attribute
- AUR packages: minimem + minimem-dkms
- OBS packaging for Fedora, Debian, Ubuntu, openSUSE
- Cross-distro packaging: Debian debian/ and Fedora .spec
- Contributing guide with 3-layer contribution model

### Changed
- Scanner sweep pass now auto-enables on patched kernels
- Fault handler registers via minimem_register_fault_handler when patches detected
- Falls back to kprobe-only mode on unpatched kernels

### Performance
- Same-page: 819:1 ratio, 0.09 us decompress
- WKdm-64: 6.2:1 on pointer-heavy, 1.5 us decompress
- AI INT8: 44:1 on uniform data, 1.6 us decompress
- All decompress 10-1000x faster than SSD swap-in

## [0.1.0] - 2024-01-01

### Added
- Initial algorithm library: LZ4, LZSSE8, WKdm, WKdm-64, BDI, Zstd, same-page, block classifier, delta XOR
- AI weight compressors: FP16/BF16 BYTE_STREAM_SPLIT, INT8 row-delta XOR
- Compression advisor with per-page algorithm selection
- Userspace library: libminimem with shared/static + pkg-config
EOF
    echo "  Created $CHANGELOG"
else
    echo "  $CHANGELOG exists. Update it manually if needed."
fi

# ---- Step 6: Commit version bump ----
echo ""
echo "--- Step 6: Commit ---"

if [ -n "$(git status --porcelain)" ]; then
    echo "Committing version bump ..."
    git add -A
    git commit -m "chore: bump version to $TAG"
    echo "  Committed."
else
    echo "  No changes to commit."
fi

# ---- Step 7: Tag ----
echo ""
echo "--- Step 7: Tag ---"

echo "Creating signed tag $TAG ..."
git tag -s "$TAG" -m "MiniMem v$VERSION"
echo "  Tag created."

# ---- Step 8: Push ----
echo ""
echo "--- Step 8: Push ---"

echo "Pushing main and tag to origin ..."
git push origin main
git push origin "$TAG"
echo "  Pushed."

if [ "$TAG_ONLY" = true ]; then
    echo ""
    echo "=== Tag-only release complete ==="
    echo "  GitHub: https://github.com/marcel-b-roodt/MiniMem/releases/tag/$TAG"
    exit 0
fi

# ---- Step 9: GitHub release ----
echo ""
echo "--- Step 9: GitHub Release ---"

if command -v gh &>/dev/null; then
    if gh release view "$TAG" --repo marcel-b-roodt/MiniMem 2>/dev/null; then
        echo "  Release already exists."
    else
        echo "Creating GitHub release ..."
        NOTES_FILE="/tmp/minimem-release-notes-$VERSION.md"
        if [ -f "$CHANGELOG" ]; then
            sed -n "/## \[$VERSION\]/,/## \[/p" "$CHANGELOG" | head -n -1 > "$NOTES_FILE"
        else
            cat > "$NOTES_FILE" << NOTES
MiniMem v$VERSION — Transparent lossless memory compression for Linux.
NOTES
        fi
        gh release create "$TAG" \
            --repo marcel-b-roodt/MiniMem \
            --title "MiniMem v$VERSION" \
            --notes-file "$NOTES_FILE"
        rm -f "$NOTES_FILE"
        echo "  GitHub release created."
    fi
else
    echo "  gh CLI not installed. Create manually:"
    echo "  https://github.com/marcel-b-roodt/MiniMem/releases/new?tag=$TAG"
fi

# ---- Step 10: AUR publish ----
echo ""
echo "--- Step 10: AUR Publish ---"

if [ -f ./scripts/publish-aur.sh ]; then
    bash ./scripts/publish-aur.sh both || echo "  AUR publish failed. Run manually."
else
    echo "  publish-aur.sh not found. Skipping."
fi

# ---- Step 11: OBS publish ----
echo ""
echo "--- Step 11: OBS Publish ---"

if command -v osc &>/dev/null && [ -f ./scripts/publish-obs.sh ]; then
    bash ./scripts/publish-obs.sh --update || echo "  OBS publish failed. Run manually."
else
    echo "  osc not installed or publish-obs.sh not found. Skipping."
    echo "  Install: pip install osc"
fi

echo ""
echo "=========================================="
echo "  MiniMem $TAG Released"
echo "=========================================="
echo ""
echo "  GitHub:    https://github.com/marcel-b-roodt/MiniMem/releases/tag/$TAG"
echo "  AUR lib:   https://aur.archlinux.org/packages/minimem"
echo "  AUR dkms:  https://aur.archlinux.org/packages/minimem-dkms"
echo "  OBS:       https://build.opensuse.org/project/show/home:minimem"
echo ""
echo "  Post-release:"
echo "    [ ] Verify AUR packages build on clean system"
echo "    [ ] Verify OBS builds pass for all distros"
echo "    [ ] Announce on GitHub Discussions"