#!/bin/bash
# scripts/publish-obs.sh — Set up and publish MiniMem on Open Build Service
#
# OBS builds packages for Fedora, Debian, Ubuntu, openSUSE simultaneously.
# This is the one-stop shop for cross-distro distribution.
#
# Prerequisites:
#   - osc (OBS command-line client) installed and configured
#   - OBS account at https://build.opensuse.org/
#   - Set OBS_USER to your OBS username (or export it)
#
# First-time setup:
#   1. Install osc: pip install osc
#   2. Run: osc setupapi   (or edit ~/.oscrc with your credentials)
#   3. Set your username:  export OBS_USER=your-obs-username
#   4. Run: ./scripts/publish-obs.sh --setup
#
# Usage: ./scripts/publish-obs.sh [--setup|--update|--check]

set -euo pipefail

VERSION="0.6.0"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OBS_USER="${OBS_USER:?Set OBS_USER to your OBS username. Example: export OBS_USER=marcelbroodt}"
OBS_PROJECT="home:${OBS_USER}"
PACKAGING_DIR="$PROJECT_DIR/packaging"

ACTION="${1:---setup}"

echo "=== MiniMem OBS Publish ==="
echo "  Version: $VERSION"
echo "  Project: $OBS_PROJECT"
echo ""

if ! command -v osc &>/dev/null; then
    echo "Error: osc not installed."
    echo "Install: pip install osc"
    echo "  or:    zypper install osc  (openSUSE)"
    echo "  or:    dnf install osc    (Fedora)"
    echo "Account: https://build.opensuse.org/"
    exit 1
fi

if [ ! -f ~/.oscrc ]; then
    echo "Error: ~/.oscrc not found. Run 'osc setupapi' first or create it:"
    echo ""
    echo "  [general]"
    echo "  apiurl = https://api.opensuse.org"
    echo ""
    echo "  [https://api.opensuse.org]"
    echo "  user = $OBS_USER"
    echo "  pass = <your-password-or-api-token>"
    exit 1
fi

case "$ACTION" in
    --setup)
        echo "Creating OBS project ..."

        PRJ_XML="/tmp/minimem-obs-project.xml"
        cat > "$PRJ_XML" << PRJ_EOF
<project name="$OBS_PROJECT">
  <title>MiniMem - Transparent Memory Compression</title>
  <description>
MiniMem provides transparent, lossless memory compression at the OS kernel
and GPU driver level. This project provides the userspace library and DKMS
kernel module packages.
  </description>
  <repository name="Fedora_Rawhide">
    <path project="Fedora:Rawhide" repository="standard"/>
    <arch>x86_64</arch>
    <arch>aarch64</arch>
  </repository>
  <repository name="Fedora_41">
    <path project="Fedora:41" repository="standard"/>
    <arch>x86_64</arch>
    <arch>aarch64</arch>
  </repository>
  <repository name="Debian_Testing">
    <path project="Debian:Testing" repository="main"/>
    <arch>x86_64</arch>
    <arch>aarch64</arch>
  </repository>
  <repository name="Ubuntu_24.04">
    <path project="Ubuntu:24.04" repository="standard"/>
    <arch>x86_64</arch>
    <arch>aarch64</arch>
  </repository>
  <repository name="openSUSE_Tumbleweed">
    <path project="openSUSE:Factory" repository="standard"/>
    <arch>x86_64</arch>
    <arch>aarch64</arch>
  </repository>
</project>
PRJ_EOF

        if osc meta prj "$OBS_PROJECT" &>/dev/null; then
            echo "Project $OBS_PROJECT already exists, updating ..."
            osc meta prj "$OBS_PROJECT" -f "$PRJ_XML"
        else
            echo "Creating project $OBS_PROJECT ..."
            osc meta prj "$OBS_PROJECT" -f "$PRJ_XML"
        fi

        rm -f "$PRJ_XML"

        echo "Creating minimem package in OBS ..."
        osc mkpac "$OBS_PROJECT/minimem" 2>/dev/null || true

        WORK_DIR="/tmp/minimem-obs-work"
        rm -rf "$WORK_DIR"
        mkdir -p "$WORK_DIR"
        osc checkout "$OBS_PROJECT" "minimem" -o "$WORK_DIR/minimem" 2>/dev/null || {
            mkdir -p "$WORK_DIR/minimem"
            osc add "$WORK_DIR/minimem" 2>/dev/null || true
        }

        echo "Copying packaging files ..."
        cp "$PACKAGING_DIR/fedora/minimem.spec" "$WORK_DIR/minimem/"
        cp "$PACKAGING_DIR/debian/control" "$WORK_DIR/minimem/debian.control"
        cp "$PACKAGING_DIR/debian/rules" "$WORK_DIR/minimem/debian.rules"
        cp "$PACKAGING_DIR/debian/changelog" "$WORK_DIR/minimem/debian.changelog"
        cp "$PACKAGING_DIR/debian/compat" "$WORK_DIR/minimem/debian.compat"
        cp "$PACKAGING_DIR/debian/copyright" "$WORK_DIR/minimem/debian.copyright"
        cp "$PACKAGING_DIR/obs/_service" "$WORK_DIR/minimem/"

        (cd "$WORK_DIR/minimem" && \
            osc addremove && \
            osc commit -m "Initial MiniMem $VERSION packaging")

        echo ""
        echo "OBS project created. Monitor builds at:"
        echo "  https://build.opensuse.org/project/show/$OBS_PROJECT"
        ;;

    --update)
        echo "Updating OBS packaging ..."
        WORK_DIR="/tmp/minimem-obs-work/minimem"

        if [ ! -d "$WORK_DIR" ]; then
            echo "Checking out OBS package ..."
            mkdir -p "$WORK_DIR"
            osc checkout "$OBS_PROJECT" "minimem" -o "$WORK_DIR"
        fi

        cp "$PACKAGING_DIR/fedora/minimem.spec" "$WORK_DIR/"
        cp "$PACKAGING_DIR/debian/control" "$WORK_DIR/debian.control"
        cp "$PACKAGING_DIR/debian/rules" "$WORK_DIR/debian.rules"
        cp "$PACKAGING_DIR/debian/changelog" "$WORK_DIR/debian.changelog"
        cp "$PACKAGING_DIR/debian/compat" "$WORK_DIR/debian.compat"
        cp "$PACKAGING_DIR/debian/copyright" "$WORK_DIR/debian.copyright"
        cp "$PACKAGING_DIR/obs/_service" "$WORK_DIR/"

        (cd "$WORK_DIR" && \
            osc addremove && \
            osc commit -m "Update MiniMem to $VERSION")

        echo "OBS packaging updated."
        ;;

    --check)
        echo "Checking OBS build status ..."
        osc results "$OBS_PROJECT" "minimem" 2>/dev/null || {
            echo "Could not fetch build status."
            echo "Check manually: https://build.opensuse.org/project/show/$OBS_PROJECT"
        }
        ;;

    *)
        echo "Usage: $0 [--setup|--update|--check]"
        echo ""
        echo "  --setup  : Create OBS project and initial packages"
        echo "  --update : Push updated packaging to OBS"
        echo "  --check  : Check build status"
        echo ""
        echo "Environment:"
        echo "  OBS_USER : Your OBS username (required)"
        exit 1
        ;;
esac