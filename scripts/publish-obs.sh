#!/bin/bash
# scripts/publish-obs.sh — Publish MiniMem on Open Build Service
#
# OBS builds packages for Debian, Ubuntu, Fedora, and openSUSE simultaneously.
# Arch Linux is handled separately via AUR (see publish-aur.sh).
#
# This script generates a source tarball from git and uploads it along
# with all packaging files. OBS cannot reliably download from GitHub,
# so we upload the tarball directly.
#
# First-time setup (manual, one-off):
#   1. pip install osc
#   2. Create ~/.config/osc/oscrc with your credentials
#   3. Create project:  osc meta prj home:YOUR_USER -e
#   4. Create package:  osc checkout home:YOUR_USER
#                      cd home:YOUR_USER && osc mkpac minimem
#
# After setup:
#   OBS_USER=yourname ./scripts/publish-obs.sh --update
#   OBS_USER=yourname ./scripts/publish-obs.sh --check
#   ./scripts/publish-obs.sh --setup-guide

set -euo pipefail

VERSION="0.9.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OBS_USER="${OBS_USER:-}"
OBS_PROJECT="home:${OBS_USER}"
PACKAGING_DIR="$PROJECT_DIR/packaging"

ACTION="${1:---setup-guide}"

case "$ACTION" in
    --setup-guide)
        echo "=== MiniMem OBS Setup Guide ==="
        echo ""
        echo "1. Install osc:"
        echo "   pip install osc"
        echo ""
        echo "2. Create ~/.config/osc/oscrc:"
        echo ""
        echo "   [general]"
        echo "   apiurl=https://api.opensuse.org"
        echo ""
        echo "   [https://api.opensuse.org]"
        echo "   user=YOUR_OBS_USERNAME"
        echo "   pass=YOUR_OBS_PASSWORD"
        echo ""
        echo "3. Create your OBS project:"
        echo "   osc meta prj home:YOUR_OBS_USERNAME -e"
        echo ""
        echo "   In the editor, paste this XML (replace YOUR_OBS_USERNAME):"
        echo ""
        cat << 'PRJ_XML'
   <project name="home:YOUR_OBS_USERNAME">
     <title>MiniMem - Transparent Memory Compression</title>
     <description>
   MiniMem provides transparent, lossless memory compression at the OS kernel
   and GPU driver level. This project provides the userspace library and DKMS
   kernel module packages.
     </description>
     <repository name="Debian_12">
       <path project="Debian:12" repository="standard"/>
       <arch>x86_64</arch>
       <arch>aarch64</arch>
     </repository>
     <repository name="Ubuntu_24.04">
       <path project="Ubuntu:24.04" repository="standard"/>
       <arch>x86_64</arch>
       <arch>aarch64</arch>
     </repository>
     <repository name="Fedora_41">
       <path project="Fedora:41" repository="standard"/>
       <arch>x86_64</arch>
       <arch>aarch64</arch>
     </repository>
     <repository name="openSUSE_Tumbleweed">
       <path project="openSUSE:Factory" repository="standard"/>
       <arch>x86_64</arch>
       <arch>aarch64</arch>
     </repository>
   </project>
PRJ_XML
        echo ""
        echo "4. Create the minimem package:"
        echo "   osc checkout home:YOUR_OBS_USERNAME"
        echo "   cd home:YOUR_OBS_USERNAME && osc mkpac minimem"
        echo ""
        echo "5. Run this script to upload packaging:"
        echo "   OBS_USER=yourname ./scripts/publish-obs.sh --update"
        echo ""
        echo "6. Check build status:"
        echo "   OBS_USER=yourname ./scripts/publish-obs.sh --check"
        ;;

    --update)
        if [ -z "$OBS_USER" ]; then
            echo "Error: Set OBS_USER to your OBS username."
            echo "  OBS_USER=yourname $0 --update"
            exit 1
        fi

        if ! command -v osc &>/dev/null; then
            echo "Error: osc not installed. Run: pip install osc"
            exit 1
        fi

        echo "=== Updating OBS packaging for $OBS_PROJECT/minimem ==="

        WORK_DIR="/tmp/minimem-obs-work"
        rm -rf "$WORK_DIR"
        mkdir -p "$WORK_DIR"

        # Generate source tarball from git archive
        TARBALL_NAME="minimem-${VERSION}.tar.gz"
        echo "Generating source tarball $TARBALL_NAME ..."
        git -C "$PROJECT_DIR" archive --format=tar.gz \
            --prefix="MiniMem-${VERSION}/" \
            -o "$WORK_DIR/$TARBALL_NAME" \
            "v${VERSION}" 2>/dev/null || {
            echo "Error: Git tag v${VERSION} not found."
            echo "Create it first: git tag -s v${VERSION} -m 'MiniMem v${VERSION}'"
            exit 1
        }

        echo "Checking out OBS package ..."
        osc checkout "$OBS_PROJECT" "minimem" -o "$WORK_DIR/minimem"

        echo "Copying packaging files ..."
        cp "$WORK_DIR/$TARBALL_NAME" "$WORK_DIR/minimem/"
        cp "$PACKAGING_DIR/fedora/minimem.spec" "$WORK_DIR/minimem/"
        cp "$PACKAGING_DIR/fedora/minimem-rpmlintrc" "$WORK_DIR/minimem/"
        cp "$PACKAGING_DIR/debian/control" "$WORK_DIR/minimem/debian.control"
        cp "$PACKAGING_DIR/debian/rules" "$WORK_DIR/minimem/debian.rules"
        cp "$PACKAGING_DIR/debian/changelog" "$WORK_DIR/minimem/debian.changelog"
        cp "$PACKAGING_DIR/debian/copyright" "$WORK_DIR/minimem/debian.copyright"
        cp "$PACKAGING_DIR/debian/libminimem0.install" "$WORK_DIR/minimem/debian.libminimem0.install"
        cp "$PACKAGING_DIR/debian/libminimem-dev.install" "$WORK_DIR/minimem/debian.libminimem-dev.install"
        cp "$PACKAGING_DIR/debian/minimem-dkms.install" "$WORK_DIR/minimem/debian.minimem-dkms.install"
        cp "$PACKAGING_DIR/debian/minimem-dkms-systemd.install" "$WORK_DIR/minimem/debian.minimem-dkms-systemd.install"
        cp "$PACKAGING_DIR/debian/minimem-cli.install" "$WORK_DIR/minimem/debian.minimem-cli.install"
        cp "$PACKAGING_DIR/debian/minimem-dkms-systemd.postinst" "$WORK_DIR/minimem/debian.minimem-dkms-systemd.postinst"
        cp "$PACKAGING_DIR/debian/minimem-dkms-systemd.prerm" "$WORK_DIR/minimem/debian.minimem-dkms-systemd.prerm"
        cp "$PACKAGING_DIR/debian/minimem.dsc" "$WORK_DIR/minimem/"
        # Do NOT copy _service — we upload the tarball directly instead of
        # letting OBS try to fetch it from GitHub (which often fails).
        # If you need OBS to regenerate the tarball, copy _service too and
        # remove the local tarball.

        echo "Files to upload:"
        ls -la "$WORK_DIR/minimem/"

        (cd "$WORK_DIR/minimem" && \
            osc addremove && \
            osc commit -m "Update MiniMem to $VERSION")

        echo ""
        echo "OBS packaging updated."
        echo "Monitor: https://build.opensuse.org/project/show/$OBS_PROJECT"
        ;;

    --check)
        if [ -z "$OBS_USER" ]; then
            echo "Error: Set OBS_USER to your OBS username."
            echo "  OBS_USER=yourname $0 --check"
            exit 1
        fi

        if ! command -v osc &>/dev/null; then
            echo "Error: osc not installed."
            exit 1
        fi

        echo "=== OBS Build Status for $OBS_PROJECT/minimem ==="
        osc results "$OBS_PROJECT" "minimem" 2>/dev/null || {
            echo "Could not fetch build status."
            echo "Check manually: https://build.opensuse.org/project/show/$OBS_PROJECT"
        }
        ;;

    *)
        echo "Usage: $0 [--setup-guide|--update|--check]"
        echo ""
        echo "  --setup-guide  : Print first-time OBS setup instructions"
        echo "  --update       : Generate tarball and push packaging to OBS"
        echo "  --check        : Check build status"
        echo ""
        echo "Environment:"
        echo "  OBS_USER : Your OBS username (required for --update and --check)"
        exit 1
        ;;
esac