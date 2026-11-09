#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 - <<'PY'
p = "tools/mk-mesa.sh"
s = open(p).read()
old = """	ninja -C $BUILD src/gallium/drivers/freedreno/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/freedreno/air_run out/air_run"""
new = """	if ninja -C $BUILD src/gallium/drivers/freedreno/air_run >> ninja.log; then
		cp $BUILD/src/gallium/drivers/freedreno/air_run out/air_run
	else
		echo "mk-mesa.sh: air_run did not build against Mesa $VERSION; the library below is what Metal uses" | tee -a ninja.log
	fi"""
assert old in s, "air_run block not found"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("mk-mesa patched: air_run optional")
PY
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-final4.log
rm -rf xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -8 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | grep -iE "air|OSMesa" | head
