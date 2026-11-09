#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
M=mesa-25.0.7
D=$M/src/gallium/drivers/freedreno/ir3
# remove self-referential / broken symlinks we may have created
for f in "$D"/*.h; do
  [ -L "$f" ] || continue
  tgt=$(readlink "$f")
  case "$tgt" in
    *"$M"*) continue ;;
  esac
  # if it points back to itself (loop), drop it
  if [ "$(realpath -m "$f" 2>/dev/null)" = "$(realpath -m "$D/$(basename "$f")" 2>/dev/null)" ]; then :; fi
done
rm -f "$D/ir3_descriptor.h" 2>/dev/null || true
# copy any plain headers air_metal's chain needs
for h in freedreno_dev_info.h ir3.h freedreno_common.h instr-a3xx.h; do
  src=$(find "$M/src" -name "$h" 2>/dev/null | head -1)
  [ -n "$src" ] && [ ! -e "$D/$h" ] && cp "$src" "$D/$h" && echo "copied $h"
done
for i in $(seq 1 10); do
  ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib2.log 2>&1
  rc=$?
  [ $rc -eq 0 ] && { echo "BUILD OK after $i"; break; }
  miss=$(grep -oE "fatal error: '[^']+' file not found" /tmp/lib2.log | head -1 | sed "s/fatal error: '//; s/' file not found//")
  if [ -z "$miss" ]; then echo "stop iter $i (no missing hdr)"; tail -10 /tmp/lib2.log; break; fi
  base=$(basename "$miss")
  src=$(find "$M/src" -name "$base" 2>/dev/null | head -1)
  [ -n "$src" ] && cp -f "$src" "$D/$miss" && echo "iter $i: copied $miss"
done
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
