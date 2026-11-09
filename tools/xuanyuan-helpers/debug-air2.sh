#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
bash -x tools/mk-mesa.sh > /tmp/air5.log 2>&1
echo "rc=$?"
echo "=== lines: $(wc -l < /tmp/air5.log) ==="
echo "--- head ---"
head -30 /tmp/air5.log
echo "--- tail ---"
tail -12 /tmp/air5.log
echo "=== outs ==="
ls -la xnu-deps/build/mesa/out 2>&1 | grep -iE "air|redwood|OSMesa" | head
