#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
W=$ROOT/xnu-deps/build/mesa
M=$W/out
echo "=== install-name tools available ==="
ls "$ROOT/xnu-deps/host/bin" | grep -iE "install-name|ldid|codesign" | head
echo "=== current install names ==="
llvm-objdump --macho --dylib-id "$M/libOSMesa.8.dylib" 2>/dev/null | head -3
llvm-objdump --macho --dylib-id "$M/libredwoodair.dylib" 2>/dev/null | head -3
echo "=== fix install name of libOSMesa and the AIR lib ==="
TOOL=$(ls "$ROOT"/xnu-deps/host/bin/*install-name-tool 2>/dev/null | head -1)
echo "tool: $TOOL"
if [ -n "$TOOL" ]; then
  "$TOOL" -id /redwood/lib/libOSMesa.8.dylib "$M/libOSMesa.8.dylib" && echo "osm id set"
  "$TOOL" -id /redwood/lib/libredwoodair.dylib "$M/libredwoodair.dylib" && echo "air id set"
else
  echo "no install-name tool; try llvm-objcopy --redefine-sym? skip"
fi
llvm-objdump --macho --dylib-id "$M/libOSMesa.8.dylib" 2>/dev/null | head -3
echo "=== rebuild image ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-fixosmesa.log 2>&1
echo "build rc=$?"
tail -5 /mnt/x/ios/logs/root-fixosmesa.log
ls -la "$ROOT/userland/root.apfs"
