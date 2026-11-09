#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
bash tools/mk-mesa.sh > /tmp/airbuild2.log 2>&1
echo rc=$?
tail -15 /tmp/airbuild2.log
echo "=== air outputs ==="
ls -la xnu-deps/build/mesa/out/ 2>/dev/null | grep -iE "air|redwood" | head
