#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
echo "=== air_metal.c 440-478 ==="
sed -n '440,478p' tools/mesa/air/air_metal.c
echo "=== libair symbols with link ==="
nm -g xnu-deps/build/mesa/build-softpipe-freedreno/src/compiler/air/libair.a 2>/dev/null | grep -i "link" | head -10
echo "=== nir_air.h mentions of linked ==="
grep -nE "linked" tools/mesa/air/nir_air.h | head -10
