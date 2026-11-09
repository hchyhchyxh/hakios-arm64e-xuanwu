#!/bin/bash
# HakiOS WSL setup: native-fs checkout + QEMU/aarch64 firmware + loader toolchain
set -e
echo "=== 1. rsync checkout to WSL native fs ==="
mkdir -p "$HOME/hakios"
rsync -a --delete /mnt/x/ios/hakios/ "$HOME/hakios/"
du -sh "$HOME/hakios"
echo "=== 2. apt update ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq 2>&1 | tail -3
echo "=== 3. apt install ==="
apt-get install -y -qq qemu-system-arm qemu-efi-aarch64 lld llvm lz4 p7zip-full 2>&1 | tail -15
echo "=== 4. verify ==="
qemu-system-aarch64 --version | head -1
ls -la /usr/share/AAVMF/ 2>/dev/null | head -6
command -v lld-link || echo "lld-link MISSING"
command -v 7zz || command -v 7z || echo "7z MISSING"
echo "=== done ==="
