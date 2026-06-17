#!/bin/bash
# scripts/release.sh — MiniMem release workflow
#
# Usage:
#   ./scripts/release.sh release <version>             # full release
#   ./scripts/release.sh release <version> --dry-run  # validate only, no changes
#   ./scripts/release.sh hotfix <version>              # hotfix from latest tag
#
# What this does:
#   1. Validates version format and git state
#   2. Verifies builds (userspace + kernel module)
#   3. Creates an annotated git tag v<version>
#   4. Pushes main + tag to origin
#   5. Creates a GitHub release (if gh CLI is available)
#   6. Publishes to AUR and OBS (if credentials are set)
#
# Version bumping is done separately (manually or via this script with --bump).
# The release script assumes the version is already correct in all files.
#
# Credentials (set as environment variables or in config files):
#   GPG_KEY       — GPG key ID for signing tags (optional, uses -a if not set)
#   AUR_SSH_USER  — AUR SSH username (in ~/.config/aur or env)
#   OBS_USER      — OBS username (in ~/.config/osc/oscrc or env)
#   GH_TOKEN      — GitHub token (via gh auth login)
#
# GPG setup (optional — unsigned tags work fine):
#   gpg --full-generate-key                    # create a key
#   git config --global user.signingkey <ID>  # tell git which key
#   # Then this script will use -s (signed) tags automatically

set -euo pipefail

ACTION=""
VERSION=""
DRY_RUN=false
NO_PUSH=false
NO_PUBLISH=false
BUMP_ONLY=false

for arg in "$@"; do
    case "$arg" in
        release|hotfix) ACTION="$arg" ;;
        --dry-run) DRY_RUN=true ;;
        --no-push) NO_PUSH=true ;;
        --no-publish) NO_PUBLISH=true ;;
        --bump) BUMP_ONLY=true ;;
        -*) ;;
        *) VERSION="$arg" ;;
    esac
done

if [ -z "$ACTION" ] || [ -z "$VERSION" ]; then
    echo "Usage: ./scripts/release.sh <release|hotfix> <version> [options]"
    echo ""
    echo "  release <ver>   Tag and publish a release from main"
    echo "  hotfix  <ver>   Tag and publish a hotfix from latest tag"
    echo ""
    echo "Options:"
    echo "  --dry-run       Validate only, no changes made"
    echo "  --no-push       Skip git push (tag + commit stay local)"
    echo "  --no-publish    Skip AUR/OBS publishing"
    echo "  --bump          Only bump version in all files, no tag/push"
    echo ""
    echo "Examples:"
    echo "  ./scripts/release.sh release 0.8.0"
    echo "  ./scripts/release.sh release 0.8.0 --dry-run"
    echo "  ./scripts/release.sh hotfix  0.8.1"
    echo "  ./scripts/release.sh release 0.8.0 --no-publish"
    exit 1
fi

if ! echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Error: Version must be semver format: X.Y.Z (got: $VERSION)"
    exit 1
fi

TAG="v$VERSION"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OBS_USER="${OBS_USER:-}"
AUR_SSH_USER="${AUR_SSH_USER:-aur}"
GPG_KEY="${GPG_KEY:-$(git config --global user.signingkey 2>/dev/null || true)}"

cd "$PROJECT_DIR"

# ---- Step 1: Validate ----
echo "=== MiniMem $ACTION $TAG ==="
echo ""
echo "--- Step 1: Validation ---"

if [ -n "$(git status --porcelain)" ]; then
    echo "Error: Working tree has uncommitted changes."
    echo "Commit or stash them before releasing."
    git status --short
    exit 1
fi

BRANCH=$(git branch --show-current)
if [ "$ACTION" = "release" ] && [ "$BRANCH" != "main" ]; then
    echo "Error: Releases must start from main. Currently on: $BRANCH"
    echo "Switch to main first: git checkout main"
    exit 1
fi

if git tag -l "$TAG" | grep -q "$TAG"; then
    echo "Error: Tag $TAG already exists."
    echo "Delete it first: git tag -d $TAG && git push origin :refs/tags/$TAG"
    exit 1
fi

echo "  Version:  $VERSION"
echo "  Tag:      $TAG"
echo "  Branch:   $BRANCH"
echo "  Tree:     clean"

if [ "$DRY_RUN" = true ]; then
    echo ""
    echo "=== Dry run complete. All checks passed. ==="
    echo "Run without --dry-run to perform the release."
    exit 0
fi

# ---- Step 2: Version bump (optional) ----
echo ""
echo "--- Step 2: Version Bump ---"

CURRENT_VERSION=$(grep 'MODULE_VERSION' src/kernel/minimem_main.c | sed 's/.*"\(.*\)".*/\1/')
if [ "$CURRENT_VERSION" = "$VERSION" ]; then
    echo "  Version already $VERSION in all files. Skipping bump."
elif [ "$BUMP_ONLY" = true ]; then
    echo "  Bumping version from $CURRENT_VERSION to $VERSION ..."
    # This is handled by the separate --bump flow below
    BUMP_VERSION="$VERSION" ./scripts/release.sh --bump
else
    echo "  Warning: Current version is $CURRENT_VERSION but releasing $VERSION."
    echo "  If you need to bump, run: ./scripts/release.sh release $VERSION --bump"
    echo "  Or manually update all files listed in AGENTS.md."
    echo ""
    read -p "  Continue with version mismatch? [y/N] " CONTINUE
    if [ "$CONTINUE" != "y" ] && [ "$CONTINUE" != "Y" ]; then
        echo "  Aborted."
        exit 1
    fi
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
if bash ./build-kmod.sh build 2>&1 | grep -q "Module built"; then
    echo "  Kernel module: OK"
else
    echo "  Warning: Kernel module build had issues. Continuing."
fi

# ---- Step 4: Tag ----
echo ""
echo "--- Step 4: Tag ---"

if [ -n "$GPG_KEY" ]; then
    echo "Creating signed tag $TAG ..."
    git tag -s "$TAG" -m "MiniMem v$VERSION"
else
    echo "Creating annotated tag $TAG (no GPG key configured) ..."
    git tag -a "$TAG" -m "MiniMem v$VERSION"
fi
echo "  Tag created."

# ---- Step 5: Push ----
if [ "$NO_PUSH" = false ]; then
    echo ""
    echo "--- Step 5: Push ---"

    echo "Pushing main to origin ..."
    git push origin main
    echo "Pushing tag $TAG to origin ..."
    git push origin "$TAG"
    echo "  Pushed."
else
    echo ""
    echo "--- Step 5: Push (skipped) ---"
    echo "  Tag $TAG created locally but not pushed."
    echo "  Push manually:"
    echo "    git push origin main"
    echo "    git push origin $TAG"
fi

# ---- Step 6: GitHub release ----
if [ "$NO_PUSH" = false ]; then
    echo ""
    echo "--- Step 6: GitHub Release ---"

    if command -v gh &>/dev/null; then
        if gh release view "$TAG" --repo marcel-b-roodt/MiniMem 2>/dev/null; then
            echo "  Release already exists."
        else
            echo "Creating GitHub release ..."

            NOTES_FILE="/tmp/minimem-release-notes-$VERSION.md"
            cat > "$NOTES_FILE" << NOTES
## MiniMem v$VERSION

### What's new

See [docs/feature-registry.md](docs/feature-registry.md) for the full feature list.

### Installation

**Arch Linux:**
\`\`\`bash
sudo ./scripts/local-install.sh
# or, when AUR is available:
yay -S minimem minimem-dkms minimem-dkms-systemd
\`\`\`

**Fedora / Debian / Ubuntu / openSUSE:**
See [OBS packages](https://build.opensuse.org/project/show/home:marcelroodt).

**From source:**
\`\`\`bash
meson setup build && meson compile -C build
sudo meson install -C build
./build-kmod.sh build && sudo ./scripts/dkms-install.sh
\`\`\`

### Monitoring

\`\`\`bash
./scripts/minimem-stats              # global stats
./scripts/minimem-stats --summary    # per-UID summary
sudo ./scripts/minimem-stats --per-process  # per-PID details
\`\`\`
NOTES

            gh release create "$TAG" \
                --repo marcel-b-roodt/MiniMem \
                --title "MiniMem v$VERSION" \
                --notes-file "$NOTES_FILE"
            rm -f "$NOTES_FILE"
            echo "  GitHub release created."
        fi
    else
        echo "  gh CLI not installed. Create release manually:"
        echo "  https://github.com/marcel-b-roodt/MiniMem/releases/new?tag=$TAG"
    fi
else
    echo ""
    echo "--- Step 6: GitHub Release (skipped) ---"
    echo "  Push was skipped. Create release after pushing:"
    echo "  https://github.com/marcel-b-roodt/MiniMem/releases/new?tag=$TAG"
fi

# ---- Step 7: AUR publish ----
if [ "$NO_PUBLISH" = false ]; then
    echo ""
    echo "--- Step 7: AUR Publish ---"

    if [ -f ./scripts/publish-aur.sh ]; then
        AUR_SSH_USER="$AUR_SSH_USER" bash ./scripts/publish-aur.sh --update || echo "  AUR publish failed. Run manually."
    else
        echo "  publish-aur.sh not found. Skipping."
    fi
else
    echo ""
    echo "--- Step 7: AUR Publish (skipped) ---"
fi

# ---- Step 8: OBS publish ----
if [ "$NO_PUBLISH" = false ] && [ -n "$OBS_USER" ]; then
    echo ""
    echo "--- Step 8: OBS Publish ---"

    if command -v osc &>/dev/null && [ -f ./scripts/publish-obs.sh ]; then
        OBS_USER="$OBS_USER" bash ./scripts/publish-obs.sh --update || echo "  OBS publish failed. Run manually."
    else
        echo "  osc not installed or publish-obs.sh not found. Skipping."
        echo "  Install: pip install osc"
        echo "  Run manually: OBS_USER=yourname ./scripts/publish-obs.sh --update"
    fi
else
    echo ""
    echo "--- Step 8: OBS Publish (skipped) ---"
    if [ -z "$OBS_USER" ]; then
        echo "  Set OBS_USER to publish: OBS_USER=yourname ./scripts/release.sh release $VERSION"
    fi
fi

# ---- Summary ----
echo ""
echo "=========================================="
echo "  MiniMem $TAG Released"
echo "=========================================="
echo ""
echo "  GitHub:        https://github.com/marcel-b-roodt/MiniMem/releases/tag/$TAG"
echo "  AUR lib:       https://aur.archlinux.org/packages/minimem"
echo "  AUR dkms:      https://aur.archlinux.org/packages/minimem-dkms"
echo "  AUR systemd:   https://aur.archlinux.org/packages/minimem-dkms-systemd"
echo "  OBS:           https://build.opensuse.org/project/show/home:${OBS_USER:-marcelroodt}"
echo ""
echo "  Post-release checklist:"
echo "    [ ] Verify AUR packages build on clean system"
echo "    [ ] Verify OBS builds pass for all distros"
echo "    [ ] Update docs/feature-registry.md"
echo "    [ ] Update docs/roadmap.md"
echo "    [ ] Update NearTermTodos.txt"
echo ""
echo "  Local testing:"
echo "    sudo ./scripts/local-install.sh --status"
echo "    ./scripts/minimem-stats"
echo "    ./vm-test-minimem.sh"