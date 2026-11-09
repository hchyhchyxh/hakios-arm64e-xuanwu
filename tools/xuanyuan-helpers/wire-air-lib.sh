#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
W=$ROOT/xnu-deps/build/mesa
LIB=$W/build26/src/gallium/drivers/freedreno/libannibaleair.dylib
OUT=$W/out
mkdir -p "$OUT"
echo "=== strip -x (keep globals) ==="
llvm-strip -x -o "$OUT/libredwoodair.dylib" "$LIB"
"$ROOT/xnu-deps/host/bin/llvm-install-name-tool" -id /redwood/lib/libredwoodair.dylib "$OUT/libredwoodair.dylib" 2>/dev/null \
  || install_name_tool -id /redwood/lib/libredwoodair.dylib "$OUT/libredwoodair.dylib" 2>/dev/null \
  || echo "install_name_tool unavailable; will set via llvm-install-name-tool"
ls -la "$OUT/libredwoodair.dylib"
echo "=== rwa symbols ==="
llvm-nm -gU "$OUT/libredwoodair.dylib" 2>/dev/null | grep -c " _rwa_"
echo "=== how build.sh handles libredwoodair ==="
grep -n "redwoodair\|libredwoodair\|libredwood-gl\|root/lib" "$ROOT/build.sh" | head -12
