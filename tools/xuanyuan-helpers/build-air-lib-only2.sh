#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
M=mesa-25.0.7
A=$M/src/compiler/air/air_metal.c
echo "=== ir3 includes in air_metal.c ==="
grep -oE '#include "(ir3|freedreno)/[^"]+"' "$A" | sort -u
D=$M/src/gallium/drivers/freedreno/ir3
for inc in $(grep -oE '#include "ir3/[^"]+"' "$A" | sed 's/#include "ir3\///; s/"//'); do
  src=$M/src/freedreno/ir3/$inc
  if [ -f "$src" ] && [ ! -e "$D/$inc" ]; then
    ln -s "../../../../freedreno/ir3/$inc" "$D/$inc"
    echo "linked $inc"
  fi
done
ls -la "$D"/ir3_compiler.h 2>/dev/null
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /mnt/x/ios/logs/air-lib-only2.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED" /mnt/x/ios/logs/air-lib-only2.log | head -6
tail -6 /mnt/x/ios/logs/air-lib-only2.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
