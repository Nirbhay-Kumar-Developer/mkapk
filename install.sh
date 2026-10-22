#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -euo pipefail

# List of required system packages
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
)

# URL of the mkapk package to download
DEB_FILE="mkapk-aarch64.deb"
DEB_URL="https://github.com/Nirbhay-Kumar-Developer/mkapk/releases/latest/download/$DEB_FILE"

apt update

echo "Installing core dependencies"

apt-get install -y --no-install-recommends "${PACKAGES[@]}"

echo "Installing mkapk"
# Create a secure temporary directory and ensure it gets cleaned up on exit
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Navigate to the temp directory
cd "$TMP_DIR"

# Download the file using curl (-L follows redirects, -sS suppresses progress bar but shows errors)
curl -L -sS -O "$DEB_URL"

# Install the downloaded package
dpkg -i "$DEB_FILE"

# Resolve any potential broken dependencies automatically
apt install -f -y

echo "Installation Completed Successfully"
