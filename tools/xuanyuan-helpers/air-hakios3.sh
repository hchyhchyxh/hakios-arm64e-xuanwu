#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 - <<'PY'
p="tools/mk-mesa.sh"
s=open(p).read()
s=s.replace('mesa-25.0.7/src/redwood-drm','mesa-25.0.7/src/hakios-drm')
s=s.replace('cp -r "$ROOT"/tools/mesa/redwood-drm mesa-25.0.7/src/redwood-drm','cp -r "$ROOT"/tools/mesa/redwood-drm mesa-25.0.7/src/hakios-drm')
open(p,"w").write(s)
print("drm dir renamed in mk-mesa")
PY
grep -n "hakios-drm\|redwood-drm" tools/mk-mesa.sh | head -4
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
LOG=/mnt/x/ios/logs/air-hakios3.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -12 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | head
