#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
# 1. proxy via current gateway (for any download mk-mesa may want)
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
echo "proxy: $http_proxy"
curl -s -o /dev/null -w "github: %{http_code}\n" --max-time 20 https://raw.githubusercontent.com/superboyyy/hakios-arm64e-dada/main/README.md || true
# 2. restore nir_air.h (upstream air dir does not carry it; ours had it)
H=tools/mesa/air/nir_air.h
if [ ! -f "$H" ]; then
  B=$(ls -d /home/hcy/tools-mesa-air-backup-* 2>/dev/null | tail -1)
  if [ -n "$B" ] && [ -f "$B/nir_air.h" ]; then
    cp "$B/nir_air.h" "$H"
    echo "restored nir_air.h from $B"
  else
    echo "NO nir_air.h backup; will need one"
  fi
fi
ls -la "$H" 2>/dev/null
# 3. rebuild AIR
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-final3.log
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -8 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa" | head
