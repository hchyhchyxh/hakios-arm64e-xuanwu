#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
B=$ROOT/xnu-deps/build/mesa
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
echo "=== upgrade meson in venv ==="
"$B/venv/bin/pip" install --quiet --upgrade "meson>=1.4" 2>&1 | tail -3
"$B/venv/bin/meson" --version
rm -rf "$B"/mesa-25.2.8 "$B"/build-softpipe-freedreno "$B"/out
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-258d.log
cd "$ROOT" || exit 1
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -10 "$LOG"
echo "=== out ==="
ls -la "$B/out" 2>/dev/null | head
