#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
echo "=== ARCH / cross / meson args in K90 mk-mesa ==="
grep -nE "ARCH|isysroot|meson setup|SDK" "$ROOT/tools/mk-mesa.sh" | head -20
echo "=== meson setup block ==="
grep -n -A12 "meson setup" "$ROOT/tools/mk-mesa.sh" | head -20
