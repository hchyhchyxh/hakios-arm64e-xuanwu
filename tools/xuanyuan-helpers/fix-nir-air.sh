#!/bin/bash
# Copy the missing nir_air.h from the HakiOS mainline clone into the xuanyuan userland tree
set -e
SRC=/mnt/x/ios/hakios/tools/mesa/air
DST=/home/hcy/hakios-arm64e-xuanyuan/userland/tools/mesa/air
echo "=== compare shared AIR files ==="
for f in air_to_nir.c air_metal.h air_private.h air2nir.c meson.build air_intrinsics.c; do
  if [ -f "$SRC/$f" ] && [ -f "$DST/$f" ]; then
    a=$(sha256sum "$SRC/$f" | cut -c1-16)
    b=$(sha256sum "$DST/$f" | cut -c1-16)
    if [ "$a" = "$b" ]; then echo "$f SAME $a"; else echo "$f DIFF main=$a tree=$b"; fi
  else
    echo "$f missing one side"
  fi
done
echo "=== copy nir_air.h ==="
cp -v "$SRC/nir_air.h" "$DST/nir_air.h"
ls -la "$DST/nir_air.h"
