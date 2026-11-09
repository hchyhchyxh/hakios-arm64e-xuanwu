#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
# restore the real header we deleted by mistake
tar -xJf mesa-25.0.7.tar.xz -C /tmp mesa-25.0.7/src/gallium/drivers/freedreno/ir3/ir3_descriptor.h 2>/dev/null
cp -f /tmp/mesa-25.0.7/src/gallium/drivers/freedreno/ir3/ir3_descriptor.h mesa-25.0.7/src/gallium/drivers/freedreno/ir3/ir3_descriptor.h
ls -la mesa-25.0.7/src/gallium/drivers/freedreno/ir3/ir3_descriptor.h
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib4.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED" /tmp/lib4.log | head -6
tail -6 /tmp/lib4.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
