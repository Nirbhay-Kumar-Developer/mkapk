#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -euo pipefail

# Parse command-line options
COMPILE_FROM_SOURCE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compile)
            COMPILE_FROM_SOURCE=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--compile]"
            echo "  --compile    Clone repository and compile from source instead of downloading .deb"
            exit 0
            ;;
        *)
            echo "Error: Unknown argument '$1'" >&2
            echo "Usage: $0 [--compile]" >&2
            exit 1
            ;;
    esac
done

# Core runtime dependencies
PACKAGES=(
    "openjdk-21"
    "ndk-multilib"
    "aapt2"
    "clang"
    "binutils"
    "libzip"
    "openssl"
    "apksigner"
    "kotlin"
    "wget"
)

# Append compile-time build toolchain if compiling from source
if [[ "$COMPILE_FROM_SOURCE" == true ]]; then
    PACKAGES+=(
        "git"
        "make"
        "maven"
        "python"
    )
fi

echo "Updating package lists..."
apt update

echo "Installing required dependencies..."
apt-get install -y --no-install-recommends "${PACKAGES[@]}"

if [[ "$COMPILE_FROM_SOURCE" == true ]]; then
    echo "Compiling mkapk from source..."
    
    REPO_URL="https://github.com/Nirbhay-Kumar-Developer/mkapk.git"
    BUILD_DIR=$(mktemp -d)

    # Clean up build dir if script fails midway
    trap 'rm -rf "$BUILD_DIR"' ERR

    cd "$BUILD_DIR"
    git clone --depth 1 "$REPO_URL" .
    bash compile.sh --release

    # Locate generated deb package and install
    DEB_PACKAGE=$(find . -maxdepth 1 -name "*.deb" | head -n 1)
    if [[ -z "$DEB_PACKAGE" ]]; then
        echo "Error: Compilation did not produce a .deb package." >&2
        rm -rf "$BUILD_DIR"
        exit 1
    fi

    dpkg -i "$DEB_PACKAGE"

    # Strictly delete the build directory immediately after package installation
    cd /
    rm -rf "$BUILD_DIR"
    trap - ERR
else
    echo "Installing pre-built mkapk package..."
    
    DEB_FILE="mkapk-aarch64.deb"
    DEB_URL="https://github.com/Nirbhay-Kumar-Developer/mkapk/releases/latest/download/$DEB_FILE"

    TMP_DIR=$(mktemp -d)

    # Clean up temp dir if download or install errors out
    trap 'rm -rf "$TMP_DIR"' ERR

    cd "$TMP_DIR"
    curl -L -sS -O "$DEB_URL"
    dpkg -i "$DEB_FILE"

    # Strictly delete the temporary download directory immediately after installing
    cd /
    rm -rf "$TMP_DIR"
    trap - ERR
fi

# Resolve any missing or broken dependencies automatically
apt install -f -y

echo "Installation Completed Successfully"
