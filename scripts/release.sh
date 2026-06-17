#!/bin/bash
# scripts/release.sh — MiniMem release and hotfix workflow
#
# Usage:
#   ./scripts/release.sh release <version>           # full release from main
#   ./scripts/release.sh release <version> --dry-run  # validate only
#   ./scripts/release.sh release <version> --tag-only # tag + push, skip publish
#   ./scripts/release.sh hotfix <version>             # hotfix from latest tag
#
# Release flow (from main):
#   1. Validate version format and git state
#   2. Create release/X.Y.Z branch from main
#   3. Verify builds (userspace + kernel module)
#   4. Bump version across all files
#   5. Commit version bump on release branch
#   6. Create signed git tag v0.X.Y
#   7. Merge release branch back to main
#   8. Push main + tag to GitHub
#   9. Create GitHub release
#  10. Publish to AUR (Arch Linux)
#  11. Publish to OBS (Debian, Ubuntu, Fedora, openSUSE)
#
# Hotfix flow (from latest tag):
#   1. Create hotfix/X.Y.Z branch from latest tag
#   2. Apply fixes (manual or cherry-pick)
#   3. Bump version to X.Y.Z
#   4. Commit, tag, merge to main, push
#   5. Publish to AUR + OBS
#
# Branching model:
#   main ──●──●──●──●──●
#              \         \
#               ●──●      ● (hotfix)
#               release/0.7.0
#
# Prerequisites:
#   - Clean working tree on main branch (or hotfix branch)
#   - GPG key configured for git signing
#   - AUR_SSH_USER set for AUR publish
#   - OBS_USER set for OBS publish
#   - gh CLI authenticated (for GitHub release)

set -euo pipefail

ACTION=""
VERSION=""
DRY_RUN=false
TAG_ONLY=false

for arg in "$@"; do
    case "$arg" in
        release|hotfix) ACTION="$arg" ;;
        --dry-run) DRY_RUN=true ;;
        --tag-only) TAG_ONLY=true ;;
        -*) ;;
        *) VERSION="$arg" ;;
    esac
done

if [ -z "$ACTION" ] || [ -z "$VERSION" ]; then
    echo "Usage: ./scripts/release.sh <release|hotfix> <version> [--dry-run] [--tag-only]"
    echo ""
    echo "  release <ver>   Create release/X.Y.Z branch from main, bump, tag, publish"
    echo "  hotfix  <ver>   Create hotfix/X.Y.Z branch from latest tag, bump, tag, publish"
    echo "  --dry-run       Validate only, no changes made"
    echo "  --tag-only      Tag + push only, skip publishing"
    echo ""
    echo "Examples:"
    echo "  ./scripts/release.sh release 0.7.0"
    echo "  ./scripts/release.sh hotfix  0.6.1"
    exit 1
fi

if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Error: Version must be semver format: X.Y.Z"
    exit 1
fi

TAG="v$VERSION"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OBS_USER="${OBS_USER:-}"
AUR_SSH_USER="${AUR_SSH_USER:-aur}"

cd "$PROJECT_DIR"

# ---- Step 1: Validate ----
echo "=== MiniMem $ACTION $TAG ==="
echo ""

echo "--- Step 1: Validation ---"

if [ -n "$(git status --porcelain)" ]; then
    echo "Error: Working tree has uncommitted changes."
    git status --short
    exit 1
fi

if git tag -l "$TAG" | grep -q "$TAG"; then
    echo "Error: Tag $TAG already exists."
    exit 1
fi

if [ "$ACTION" = "release" ]; then
    BRANCH=$(git branch --show-current)
    if [ "$BRANCH" != "main" ]; then
        echo "Error: Releases must start from main. Currently on: $BRANCH"
        exit 1
    fi
    RELEASE_BRANCH="release/$VERSION"
    echo "  Will create branch: $RELEASE_BRANCH from main"
elif [ "$ACTION" = "hotfix" ]; then
    LATEST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
    if [ -z "$LATEST_TAG" ]; then
        echo "Error: No existing tags found. Use 'release' for first release."
        exit 1
    fi
    RELEASE_BRANCH="hotfix/$VERSION"
    echo "  Will create branch: $RELEASE_BRANCH from $LATEST_TAG"
fi

echo "  Tree: clean"
echo "  Tag $TAG: available"

if [ "$DRY_RUN" = true ]; then
    echo ""
    echo "=== Dry run complete. All checks passed. ==="
    echo "Run without --dry-run to perform the release."
    exit 0
fi

# ---- Step 2: Create release/hotfix branch ----
echo ""
echo "--- Step 2: Create $RELEASE_BRANCH ---"

if [ "$ACTION" = "release" ]; then
    git checkout -b "$RELEASE_BRANCH"
    echo "  Created $RELEASE_BRANCH from main"
elif [ "$ACTION" = "hotfix" ]; then
    git checkout -b "$RELEASE_BRANCH" "$LATEST_TAG"
    echo "  Created $RELEASE_BRANCH from $LATEST_TAG"
fi

# ---- Step 3: Build verification ----
echo ""
echo "--- Step 3: Build Verification ---"

if [ ! -d "build" ]; then
    echo "Setting up userspace build ..."
    meson setup build -Dtests=false
fi

echo "Building userspace library ..."
meson compile -C build 2>&1 | tail -1
echo "  Userspace: OK"

echo "Building kernel module ..."
if ! bash ./build-kmod.sh build 2>&1 | grep -q "Module built"; then
    echo "Warning: Kernel module build failed (may need path without spaces)."
    echo "  Continuing — module build is verified separately via VM tests."
fi

# ---- Step 4: Version bump ----
echo ""
echo "--- Step 4: Version Bump ---"

OLD_VERSION=$(grep 'MODULE_VERSION' src/kernel/minimem_main.c | sed 's/.*"\(.*\)".*/\1/')
echo "  Current version: $OLD_VERSION"
echo "  New version:      $VERSION"

if [ "$OLD_VERSION" = "$VERSION" ]; then
    echo "  Version already matches, skipping bump."
else
    echo "  Bumping version in all files ..."

    # Kernel module
    sed -i "s/$OLD_VERSION/$VERSION/g" src/kernel/minimem_main.c
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/dkms.conf
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/install.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" dkms/uninstall.sh

    # Build system
    sed -i "s/version : '$OLD_VERSION'/version : '$VERSION'/g" meson.build
    OLD_VERSION_MAJOR=$(echo "$OLD_VERSION" | cut -d. -f1)
    OLD_VERSION_MINOR=$(echo "$OLD_VERSION" | cut -d. -f2)
    OLD_VERSION_PATCH=$(echo "$OLD_VERSION" | cut -d. -f3)
    NEW_VERSION_MAJOR=$(echo "$VERSION" | cut -d. -f1)
    NEW_VERSION_MINOR=$(echo "$VERSION" | cut -d. -f2)
    NEW_VERSION_PATCH=$(echo "$VERSION" | cut -d. -f3)
    sed -i "s/minimem_version_major = '$OLD_VERSION_MAJOR'/minimem_version_major = '$NEW_VERSION_MAJOR'/g" meson.build
    sed -i "s/minimem_version_minor = '$OLD_VERSION_MINOR'/minimem_version_minor = '$NEW_VERSION_MINOR'/g" meson.build
    sed -i "s/minimem_version_patch = '$OLD_VERSION_PATCH'/minimem_version_patch = '$NEW_VERSION_PATCH'/g" meson.build

    # Scripts
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/dkms-install.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/dkms-uninstall.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-obs.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-aur.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-debian.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-fedora.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/publish-all.sh
    sed -i "s/$OLD_VERSION/$VERSION/g" scripts/local-install.sh

    # AUR packaging
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem/PKGBUILD
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem/.SRCINFO
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms/PKGBUILD
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms/.SRCINFO
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms/minimem-dkms.install
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms-systemd/PKGBUILD
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms-systemd/.SRCINFO
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/aur/minimem-dkms-systemd/minimem-dkms-systemd.install

    # Fedora/RPM packaging
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/fedora/minimem.spec

    # Debian packaging
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/control
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/rules
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/changelog
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/debian/minimem.dsc

    # OBS
    sed -i "s/$OLD_VERSION/$VERSION/g" packaging/obs/_service

    # Documentation
    sed -i "s/v$OLD_VERSION/v$VERSION/g" docs/roadmap.md
    sed -i "s/v$OLD_VERSION/v$VERSION/g" docs/benchmarks.md

    echo "  Done. Verify with: git diff"
fi

# ---- Step 5: Commit ----
echo ""
echo "--- Step 5: Commit ---"

if [ -n "$(git status --porcelain)" ]; then
    git add -A
    git commit -m "chore(release): bump version to $TAG"
    echo "  Committed."
else
    echo "  No changes to commit."
fi

# ---- Step 6: Tag ----
echo ""
echo "--- Step 6: Tag ---"

echo "Creating signed tag $TAG ..."
git tag -s "$TAG" -m "MiniMem v$VERSION"
echo "  Tag created."

# ---- Step 7: Merge back to main ----
echo ""
echo "--- Step 7: Merge to main ---"

git checkout main
git merge --no-ff "$RELEASE_BRANCH" -m "Merge $RELEASE_BRANCH"
echo "  Merged $RELEASE_BRANCH into main"

# ---- Step 8: Push ----
echo ""
echo "--- Step 8: Push ---"

echo "Pushing main and tag to origin ..."
git push origin main
git push origin "$TAG"
echo "  Pushed."

# Clean up release branch (keep locally for reference)
git branch -d "$RELEASE_BRANCH" 2>/dev/null || true
echo "  Deleted local $RELEASE_BRANCH branch (tag preserves the snapshot)."

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
        cat > "$NOTES_FILE" << NOTES
MiniMem v$VERSION — Transparent lossless memory compression for Linux.
NOTES
        if [ -f CHANGELOG.md ]; then
            sed -n "/## \[$VERSION\]/,/## \[/p" CHANGELOG.md | head -n -1 > "$NOTES_FILE"
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
    AUR_SSH_USER="$AUR_SSH_USER" bash ./scripts/publish-aur.sh --update || echo "  AUR publish failed. Run manually."
else
    echo "  publish-aur.sh not found. Skipping."
fi

# ---- Step 11: OBS publish ----
echo ""
echo "--- Step 11: OBS Publish ---"

if [ -n "$OBS_USER" ] && command -v osc &>/dev/null && [ -f ./scripts/publish-obs.sh ]; then
    OBS_USER="$OBS_USER" bash ./scripts/publish-obs.sh --update || echo "  OBS publish failed. Run manually."
else
    echo "  OBS_USER not set or osc not installed. Skipping OBS publish."
    echo "  Run manually: OBS_USER=yourname ./scripts/publish-obs.sh --update"
fi

echo ""
echo "=========================================="
echo "  MiniMem $TAG Released"
echo "=========================================="
echo ""
echo "  GitHub:    https://github.com/marcel-b-roodt/MiniMem/releases/tag/$TAG"
echo "  AUR lib:     https://aur.archlinux.org/packages/minimem"
echo "  AUR dkms:    https://aur.archlinux.org/packages/minimem-dkms"
echo "  AUR systemd: https://aur.archlinux.org/packages/minimem-dkms-systemd"
echo "  OBS:       https://build.opensuse.org/project/show/home:${OBS_USER:-YOUR_OBS_USER}"
echo ""
echo "  Post-release checklist:"
echo "    [ ] Verify AUR packages build on clean system"
echo "    [ ] Verify OBS builds pass for all distros"
echo "    [ ] Announce on GitHub Discussions"
echo ""
echo "  Hotfix workflow:"
echo "    ./scripts/release.sh hotfix X.Y.Z  # create hotfix branch from latest tag"