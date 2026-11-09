#!/bin/bash
# Rebuild Mesa for arm64e (the userland arch), then rebuild the GL root image.
set -o pipefail
export http_proxy=http://172.25.144.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy
export HTTPS_PROXY=$https_proxy
export THEOS=/home/hcy/theos
export HOME=/home/hcy
export ARCH=arm64e
export XNU_ARCH=arm64e
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
python3 /mnt/x/ios/scripts/patch-mkmesa-arch.py
echo "=== fresh Mesa build for arm64e ==="
rm -rf userland/xnu-deps/build/mesa/build-softpipe-freedreno userland/xnu-deps/build/mesa/out
bash userland/tools/mk-mesa.sh 2>&1 | tail -15
echo "=== mesa rc=$? ==="
ls -la userland/xnu-deps/build/mesa/out 2>/dev/null
echo "=== rebrand air hash ==="
h=$(cat userland/tools/mesa/air/* | sha256sum)
echo "$h" > userland/xnu-deps/build/mesa/out/built-from-air
cat userland/xnu-deps/build/mesa/out/built-from-air
echo "=== ARM64E MESA DONE ==="
