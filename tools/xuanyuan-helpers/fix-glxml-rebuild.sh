#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
W=$ROOT/xnu-deps/build/mesa
M=$W/out
GL=$(find "$W/mesa-26.2.5" -name "gl.xml" 2>/dev/null | head -1)
echo "gl.xml: $GL"
[ -n "$GL" ] && cp "$GL" "$M/gl.xml" && ls -la "$M/gl.xml"
echo "=== rebuild ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-metal4.log 2>&1
echo "build rc=$?"
tail -10 /mnt/x/ios/logs/root-metal4.log
ls -la "$ROOT/userland/root.apfs" 2>/dev/null
ls -la "$ROOT/userland/root/lib/libredwoodair.dylib" 2>/dev/null
ls "$ROOT/userland/root/lib" 2>/dev/null | grep -iE "metal|air" | head
