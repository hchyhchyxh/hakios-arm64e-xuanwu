#!/bin/bash
# init Mu_Basecore submodules (mipisyst, libfdt, brotli...) then rebuild xuanyuan UEFI
set -o pipefail
export http_proxy=http://172.31.128.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy HTTPS_PROXY=$https_proxy
cd /root/xuanyuan-uefi/Mu_Basecore || exit 1
echo "=== Mu_Basecore submodules ==="
git submodule update --init --depth 1 2>&1 | tail -25
echo "=== submodule update rc=$? ==="
echo "=== mipisyst present? ==="
ls MdePkg/Library/MipiSysTLib/mipisyst/library/include 2>/dev/null | head -3 || echo "MISSING"

cd /root/xuanyuan-uefi || exit 1
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
mkdir -p /mnt/x/ios/logs Build
echo "=== build ==="
python3 build_uefi.py -d xuanyuan 2>&1 | tee /mnt/x/ios/logs/uefi-build.log
brc=${PIPESTATUS[0]}
echo "=== build rc=$brc ==="
echo "=== outputs ==="
find Build -maxdepth 5 -type f \( -name 'SILICIUM_UEFI.fd' -o -name 'SILICIUM_UEFI.fd.gz' -o -name 'SILICIUM_UEFI.fd-bootshim*' \) -printf '%10s  %p\n' 2>/dev/null | head -10
ls -la Mu-xuanyuan* 2>/dev/null
echo "=== BUILD DONE (rc=$brc) ==="
