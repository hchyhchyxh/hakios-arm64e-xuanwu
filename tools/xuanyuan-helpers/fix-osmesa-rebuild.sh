#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
W=$ROOT/xnu-deps/build/mesa
M=$W/out
cd "$W" || exit 1
echo "=== build libOSMesa.8.dylib from mesa-26.2.5 (arm64e) ==="
ninja -C build26 src/gallium/targets/osmesa/libOSMesa.8.dylib > /mnt/x/ios/logs/osmesa26.log 2>&1
echo "ninja rc=$?"
tail -3 /mnt/x/ios/logs/osmesa26.log
ls -d build26/src/gallium/targets/osmesa 2>/dev/null
find build26 -name "libOSMesa*" 2>/dev/null | head -3
OS=$(find build26 -name "libOSMesa.8.dylib" | head -1)
[ -n "$OS" ] || { echo "no libOSMesa built"; exit 1; }
llvm-strip -x -o "$M/libOSMesa.8.dylib" "$OS"
"$ROOT/xnu-deps/host/bin/llvm-install-name-tool" -id /redwood/lib/libOSMesa.8.dylib "$M/libOSMesa.8.dylib" 2>/dev/null || true
llvm-nm -gU "$M/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$M/exported-gl.txt"
cp mesa-26.2.5/src/mapi/glapi/registry/gl.xml "$M/gl.xml"
ls -la "$M"/libOSMesa.8.dylib "$M"/gl.xml "$M"/exported-gl.txt
echo "=== rebuild image ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-metal3.log 2>&1
echo "build rc=$?"
tail -8 /mnt/x/ios/logs/root-metal3.log
ls -la "$ROOT/userland/root.apfs" 2>/dev/null
ls -la "$ROOT/userland/root/lib/libredwoodair.dylib" 2>/dev/null
