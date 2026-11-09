#!/bin/bash
# Build Mesa (softpipe) for the xuanyuan iOS userland: gives CoreAnimation an OpenGL ES renderer
# when booting with gl=1 on a phone with no GPU driver yet.
set -o pipefail
export http_proxy=http://172.25.144.1:12001
export https_proxy=http://172.25.144.1:12001
export HTTP_PROXY=$http_proxy
export HTTPS_PROXY=$https_proxy
export DEBIAN_FRONTEND=noninteractive
LOG=/mnt/x/ios/logs/mesa-build.log
mkdir -p /mnt/x/ios/logs

echo "=== apt deps ===" | tee -a "$LOG"
apt-get install -y -qq meson ninja-build bison flex python3-venv llvm pkg-config 2>&1 | tail -3 | tee -a "$LOG"

cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== mk-mesa.sh ===" | tee -a "$LOG"
bash userland/tools/mk-mesa.sh 2>&1 | tee -a "$LOG" | tail -80
rc=${PIPESTATUS[0]}
echo "=== mk-mesa rc=$rc ===" | tee -a "$LOG"
echo "=== outputs ===" | tee -a "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | tee -a "$LOG"
ls -la userland/xnu-deps/build/mesa/out 2>/dev/null | tee -a "$LOG"
echo "=== MESA DONE (rc=$rc) ===" | tee -a "$LOG"
