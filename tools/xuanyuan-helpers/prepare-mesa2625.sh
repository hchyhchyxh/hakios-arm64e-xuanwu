#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
K=/mnt/x/ios/work/hackios-userland-k90/hackios-xnu-userland-k90-port
SRC=/mnt/x/ios/work/mesa-26.2.5.tar.xz
B=$ROOT/xnu-deps/build/mesa
[ -f "$SRC" ] || { echo "no mesa tarball"; exit 1; }
echo "=== install K90 gens: tools/mesa (51 air files, rebase-26) + mk-mesa ==="
cp -a "$ROOT/tools/mesa" "/home/hcy/tools-mesa-old-$(date +%s)" 2>/dev/null
rm -rf "$ROOT/tools/mesa"
cp -a "$K/tools/mesa" "$ROOT/tools/mesa"
cp -a "$K/tools/mk-mesa.sh" "$ROOT/tools/mk-mesa.sh"
echo "air files: $(ls "$ROOT/tools/mesa/air" | wc -l); rebase-26 patches: $(ls "$ROOT/tools/mesa/rebase-26"/*.patch 2>/dev/null | wc -l)"
echo "=== stage tarball + PREPARE (unpack + patch only) ==="
mkdir -p "$B"
cp -f "$SRC" "$B/mesa-26.2.5.tar.xz"
rm -rf "$B/mesa-26.2.5" "$B/build-softpipe-freedreno" "$B/out"
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e
cd "$ROOT" || exit 1
PREPARE=1 VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 REBASE=26 \
  bash tools/mk-mesa.sh > /mnt/x/ios/logs/mesa2625-prepare.log 2>&1
echo "PREPARE rc=$?"
tail -12 /mnt/x/ios/logs/mesa2625-prepare.log
ls -d "$B/mesa-26.2.5" 2>/dev/null && echo "mesa-26.2.5 tree ready"
ls "$B/mesa-26.2.5/src/compiler/air" 2>/dev/null | head -8
