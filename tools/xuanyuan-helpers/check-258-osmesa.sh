#!/bin/bash
M=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa/mesa-25.2.8
grep -n "osmesa" "$M"/meson.options 2>/dev/null | head -5
echo "=== targets dirs ==="
ls "$M"/src/gallium/targets | grep -i osmesa
echo "=== meson.build osmesa option usage ==="
grep -rn "osmesa" "$M"/src/gallium/meson.build "$M"/meson.build 2>/dev/null | head -8
