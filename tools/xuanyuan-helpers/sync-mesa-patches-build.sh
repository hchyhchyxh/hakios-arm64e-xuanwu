#!/bin/bash
set -u
U=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mesa
T=/home/hcy/hakios-arm64e-xuanyuan/userland/tools/mesa
echo "=== upstream patches ==="
ls "$U"/*.patch 2>/dev/null | wc -l
cp -a "$T" /home/hcy/tools-mesa-backup-$(date +%s) 2>/dev/null
rm -rf "$T"
mkdir -p "$T"
cp -a "$U"/* "$T"/
echo "=== our tools/mesa now ==="
ls "$T" | head -12
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-final2.log
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -6 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa" | head
