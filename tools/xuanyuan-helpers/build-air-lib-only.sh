#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
M=mesa-25.0.7
L=$M/src/gallium/drivers/freedreno/ir3
[ -e "$L" ] || ln -s ../../../freedreno/ir3 "$L"
ls "$L"/ir3_compiler.h 2>/dev/null && echo "symlink ok"
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /mnt/x/ios/logs/air-lib-only.log 2>&1
echo "ninja rc=$?"
tail -12 /mnt/x/ios/logs/air-lib-only.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
