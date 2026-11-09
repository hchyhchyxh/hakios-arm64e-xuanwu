#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
K=/mnt/x/ios/work/hackios-userland-k90/hackios-xnu-userland-k90-port
echo "=== K90 tools/ top ==="
ls "$K/tools" | head -30
echo "=== copy missing scripts into our tools/ ==="
for f in "$K"/tools/*.sh "$K"/tools/*.py; do
  b=$(basename "$f")
  [ -e "$ROOT/tools/$b" ] || { cp -a "$f" "$ROOT/tools/$b"; echo "copied $b"; }
done
ls "$ROOT/tools" | grep -E "mesa|stamp" | head
echo "=== rerun PREPARE ==="
B=$ROOT/xnu-deps/build/mesa
rm -rf "$B/mesa-26.2.5"
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e
cd "$ROOT" || exit 1
PREPARE=1 VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 REBASE=26 \
  bash tools/mk-mesa.sh > /mnt/x/ios/logs/mesa2625-prepare2.log 2>&1
echo "PREPARE rc=$?"
tail -12 /mnt/x/ios/logs/mesa2625-prepare2.log
ls -d "$B/mesa-26.2.5" 2>/dev/null && echo "tree ready"
ls "$B/mesa-26.2.5/src/compiler/air" 2>/dev/null | head -6
