#!/bin/bash
# Rebuild Mesa after nir_air.h is in place
export http_proxy=http://172.25.144.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy
export HTTPS_PROXY=$https_proxy
export THEOS=/home/hcy/theos
export HOME=/home/hcy
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
rm -rf userland/xnu-deps/build/mesa/build-softpipe-freedreno
echo "=== mk-mesa ==="
bash userland/tools/mk-mesa.sh 2>&1 | tail -50
rc=${PIPESTATUS[0]}
echo "=== mk-mesa rc=$rc ==="
echo "=== outputs ==="
ls -la userland/xnu-deps/build/mesa/out 2>/dev/null
echo "=== MESA DONE (rc=$rc) ==="
