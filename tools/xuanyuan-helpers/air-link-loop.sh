#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
M=mesa-25.0.7
D=$M/src/gallium/drivers/freedreno/ir3
for i in $(seq 1 12); do
  ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib.log 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then echo "BUILD OK after $i iterations"; break; fi
  miss=$(grep -oE "fatal error: '[^']+' file not found" /tmp/lib.log | head -1 | sed "s/fatal error: '//; s/' file not found//")
  if [ -z "$miss" ]; then echo "no missing-header error; stop at iter $i"; tail -8 /tmp/lib.log; break; fi
  base=$(basename "$miss")
  found=$(find "$M/src" -name "$base" 2>/dev/null | head -1)
  if [ -z "$found" ]; then echo "cannot find $miss"; break; fi
  target="$D/$miss"
  mkdir -p "$(dirname "$target")"
  rel=$(realpath --relative-to="$(dirname "$target")" "$found")
  ln -sfn "$rel" "$target"
  echo "iter $i: linked $miss -> $found"
done
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
tail -5 /tmp/lib.log
