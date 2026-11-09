#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e
export http_proxy=http://172.31.128.1:12001 https_proxy=http://172.31.128.1:12001
# proxy may be unavailable; mesa sources are already local
bash tools/mk-mesa.sh > /tmp/airbuild.log 2>&1 <<< ""
echo rc=$?
tail -12 /tmp/airbuild.log
echo "=== outputs ==="
ls -la xnu-deps/build/mesa/out/ 2>/dev/null | head
