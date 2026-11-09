#!/bin/bash
# Build the AIR frontend library too (mk-mesa AIR=1), so userland/build.sh's hash check passes
export http_proxy=http://172.25.144.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy
export HTTPS_PROXY=$https_proxy
export THEOS=/home/hcy/theos
export HOME=/home/hcy
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== AIR=1 mk-mesa ==="
AIR=1 bash userland/tools/mk-mesa.sh 2>&1 | tail -25
rc=${PIPESTATUS[0]}
echo "=== AIR mk-mesa rc=$rc ==="
ls -la userland/xnu-deps/build/mesa/out 2>/dev/null
echo "=== AIR DONE (rc=$rc) ==="
