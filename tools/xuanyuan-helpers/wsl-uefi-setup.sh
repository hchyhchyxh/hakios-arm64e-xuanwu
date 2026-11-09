#!/bin/bash
# WSL build environment for the xuanyuan Mu-Silicium UEFI (milestone 2)
set -e
export DEBIAN_FRONTEND=noninteractive
echo "=== apt update ==="
apt-get update -qq
echo "=== apt install (Mu-Silicium deps + iasl) ==="
apt-get install -y -qq pip git mono-devel build-essential lld uuid-dev nasm \
    gcc-aarch64-linux-gnu python3 python3-git python3-pip gettext locales \
    gnupg ca-certificates python3-venv clang llvm curl acpica-tools device-tree-compiler 2>&1 | tail -6
echo "=== rsync uefi-dada tree into WSL native fs ==="
mkdir -p /root/xuanyuan-uefi
rsync -a --delete /mnt/x/ios/port/uefi-dada/ /root/xuanyuan-uefi/
du -sh /root/xuanyuan-uefi
echo "=== pip deps (tuna mirror) ==="
cd /root/xuanyuan-uefi
PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple \
PIP_TRUSTED_HOST=pypi.tuna.tsinghua.edu.cn \
python3 -m pip install --break-system-packages -r pip-requirements.txt 2>&1 | tail -8
echo "=== verify ==="
python3 -c "import edk2toolext, edk2toollib; print('edk2 python deps OK')" 2>&1
printf 'gcc: '; aarch64-linux-gnu-gcc --version 2>/dev/null | head -1
printf 'nasm: '; nasm -v 2>/dev/null
printf 'iasl: '; iasl -v 2>&1 | sed -n '2p'
echo "=== done ==="
