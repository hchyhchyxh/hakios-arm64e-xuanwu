#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
SNAP=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland
echo "=== restore tools/mesa + mk-mesa from the upstream snapshot (Mesa 25.0.7) ==="
rm -rf "$ROOT/tools/mesa"
cp -a "$SNAP/tools/mesa" "$ROOT/tools/mesa"
cp -a "$SNAP/tools/mk-mesa.sh" "$ROOT/tools/mk-mesa.sh"
grep -nE "^VERSION=|^SHA256=" "$ROOT/tools/mk-mesa.sh" | head -2
echo "tools/mesa: $(ls "$ROOT/tools/mesa" | tr '\n' ' ')"
B=$ROOT/xnu-deps/build/mesa
rm -rf "$B"/mesa-25.2.8 "$B"/build-softpipe-freedreno "$B"/out
# make the GL/OSMesa build work again (this is the state that built libOSMesa + libair.a)
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e
cd "$ROOT" || exit 1
bash tools/mk-mesa.sh > /mnt/x/ios/logs/mesa-restore.log 2>&1
echo "mk-mesa rc=$?"
tail -4 /mnt/x/ios/logs/mesa-restore.log
ls -la "$B"/out 2>/dev/null | head
