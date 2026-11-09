#!/bin/bash
# Tell userland/build.sh the AIR tree's current hash (we ship no Metal driver in this GL test image)
set -e
cd /home/hcy/hakios-arm64e-xuanyuan/userland/userland
echo "=== M definitions in build.sh ==="
grep -nE "^(M|M26|MESA)=" build.sh | head -5
echo "=== current air hash ==="
h=$(cat ../tools/mesa/air/* | sha256sum)
echo "$h"
for d in ../xnu-deps/build/mesa/out; do
  if [ -d "$d" ]; then echo "$h" > "$d/built-from-air"; echo "wrote $d/built-from-air"; fi
done
cat ../xnu-deps/build/mesa/out/built-from-air
