#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
SNAP=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mesa
# dada's air_metal.c/h provide the Metal-facing library API; HakiOS's air set lacks them
cp "$SNAP"/air/air_metal.c "$SNAP"/air/air_metal.h tools/mesa/air/ 2>/dev/null || true
cp "$SNAP"/air-metal.patch tools/mesa/ 2>/dev/null || true
echo "air files: $(ls tools/mesa/air | wc -l); patches: $(ls tools/mesa/*.patch | wc -l)"
# add air-metal.patch to mk-mesa's hash list
python3 - <<'PY'
p="tools/mk-mesa.sh"
s=open(p).read()
key='"$ROOT/tools/mesa/osmesa-es-profile.patch"'
if 'air-metal.patch' not in s:
    s=s.replace(key, '"$ROOT/tools/mesa/air-metal.patch" '+key)
open(p,"w").write(s)
print("added air-metal.patch to lists")
PY
grep -n "air-metal.patch" tools/mk-mesa.sh | head -3
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
rm -rf xnu-deps/build/mesa/mesa-25.0.7 xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
LOG=/mnt/x/ios/logs/air-hybrid.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -14 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | head
