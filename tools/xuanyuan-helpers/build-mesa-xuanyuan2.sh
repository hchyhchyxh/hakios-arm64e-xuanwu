#!/bin/bash
export http_proxy=http://172.25.144.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy HTTPS_PROXY=$https_proxy
export THEOS=/home/hcy/theos
export HOME=/home/hcy
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== xnu-clang present? ==="
ls -la userland/xnu-deps/host/bin/xnu-clang base/xnu-deps/host/bin/xnu-clang 2>&1
echo "=== run mk-mesa ==="
set -o pipefail
bash userland/tools/mk-mesa.sh 2>&1 | tee /mnt/x/ios/logs/mesa-build2.log | tail -40
rc=${PIPESTATUS[0]}
echo "=== mk-mesa rc=$rc ==="
ls -la userland/xnu-deps/build/mesa/out 2>/dev/null
echo "=== MESA DONE (rc=$rc) ==="
