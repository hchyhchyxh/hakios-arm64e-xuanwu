#!/bin/bash
M=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa/mesa-25.0.7
find "$M" -name "ir3_compiler.h" 2>/dev/null | head -3
echo "=== freedreno dir ==="
ls "$M/src/gallium/drivers/freedreno" | head -12
echo "=== ir3 dir? ==="
ls "$M/src/gallium/drivers/freedreno/ir3" 2>/dev/null | head -5
