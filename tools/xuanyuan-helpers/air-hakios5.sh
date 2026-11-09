#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
cp -n /mnt/x/ios/work/hakios-air/* tools/mesa/air/ 2>/dev/null || true
echo "air files: $(ls tools/mesa/air | wc -l)"
ls tools/mesa/air | tr '\n' ' '; echo
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
LOG=/mnt/x/ios/logs/air-hakios5.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -14 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | head
