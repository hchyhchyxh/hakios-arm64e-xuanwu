#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
M=xnu-deps/build/mesa/mesa-25.0.7
L="$M/src/gallium/drivers/freedreno/ir3"
if [ ! -e "$L" ]; then
  ln -s ../../../freedreno/ir3 "$L"
  echo "symlink created: $L -> $(readlink "$L")"
fi
ls "$L"/ir3_compiler.h 2>/dev/null
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-hybrid3.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -10 "$LOG"
echo "=== out ==="
ls -la xnu-deps/build/mesa/out 2>/dev/null | head
