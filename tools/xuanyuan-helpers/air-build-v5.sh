#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 - <<'PY'
p = "tools/mk-mesa.sh"
s = open(p).read()
old = """	echo "mk-mesa.sh: $B/out/air_run ($(stat -c %s out/air_run) bytes)\""""
new = """	[ -f out/air_run ] && echo "mk-mesa.sh: $B/out/air_run ($(stat -c %s out/air_run) bytes)""""
assert old in s, "air_run echo not found"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("echo made tolerant")
PY
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-final5.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -10 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa" | head
