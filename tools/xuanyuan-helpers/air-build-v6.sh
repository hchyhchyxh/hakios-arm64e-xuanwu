#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 /mnt/x/ios/scripts/fix-airrun-echo.py
grep -n "air_run" tools/mk-mesa.sh | head -6
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
echo "proxy=$http_proxy"
LOG=/mnt/x/ios/logs/air-v6.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -12 "$LOG"
echo "=== out ==="
ls -la xnu-deps/build/mesa/out 2>/dev/null | head -10
