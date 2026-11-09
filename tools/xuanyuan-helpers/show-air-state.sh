#!/bin/bash
D=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
ls -la "$D" | head -12
echo "=== any log ==="
for f in "$D"/*.log "$D"/mesa-25.0.7/.redwood-patches; do [ -f "$f" ] && echo "--- $f ($(stat -c%s "$f") bytes)" && tail -8 "$f"; done
echo "=== mesa tree present? ==="
ls -d "$D"/mesa-25.0.7 "$D"/build-softpipe-freedreno 2>/dev/null
echo "=== air copied into mesa? ==="
ls "$D"/mesa-25.0.7/src/compiler/air 2>/dev/null | head -5
