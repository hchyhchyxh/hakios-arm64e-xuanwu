#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
A=tools/mesa/air
cp -a "$A" /home/hcy/hakios-air-backup-$(date +%s) 2>/dev/null
rm -rf "$A"
mkdir -p "$A"
cp /mnt/x/ios/work/hakios-air/* "$A"/
echo "air files: $(ls "$A" | wc -l)"
ls "$A" | head -20
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-hakios.log
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -10 "$LOG"
echo "=== out ==="
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa" | head
