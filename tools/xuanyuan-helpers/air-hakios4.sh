#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 - <<'PY'
p="tools/mk-mesa.sh"
s=open(p).read()
n=s.count("src/redwood-drm")
s=s.replace("src/redwood-drm","src/hakios-drm")
open(p,"w").write(s)
print("renamed occurrences:",n)
PY
grep -n "drm" tools/mk-mesa.sh | head -6
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out xnu-deps/build/mesa/mesa-25.0.7
LOG=/mnt/x/ios/logs/air-hakios4.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -14 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | head
