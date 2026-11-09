#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-final.log
mkdir -p /mnt/x/ios/logs
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC"
echo "log bytes: $(stat -c%s "$LOG")"
tail -6 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa|redwood" | head
