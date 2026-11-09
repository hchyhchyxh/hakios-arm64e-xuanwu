#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
U=$ROOT/userland
M=$ROOT/xnu-deps/build/mesa/out
echo "=== ensure library in place ==="
ls -la "$M/libredwoodair.dylib"
echo "=== write hash markers so build.sh keeps this tree ==="
(cd "$ROOT" && sha256sum tools/mesa/*.patch tools/mesa/redwood-drm/* | sha256sum | cut -d' ' -f1) > "$M/built-from"
(cd "$ROOT" && cat tools/mesa/air/* | sha256sum | cut -d' ' -f1) > "$M/built-from-air"
cat "$M/built-from"; cat "$M/built-from-air"
echo "=== RADIO=0 for this image (keep payload under 63 MB) ==="
sed -i 's/RADIO=1/RADIO=0/' /mnt/x/ios/scripts/build-userland-gl.sh
grep -n "RADIO=" /mnt/x/ios/scripts/build-userland-gl.sh | head -2
echo "=== rebuild root image ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-metal.log 2>&1
echo "build rc=$?"
tail -6 /mnt/x/ios/logs/root-metal.log
ls -la "$U/root.apfs"
ls -la "$U/root/lib/libredwoodair.dylib" 2>/dev/null
ls "$U/root/lib" | grep -iE "metal|air" | head
