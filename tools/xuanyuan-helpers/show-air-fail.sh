#!/bin/bash
D=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
echo "=== meson-setup tail ==="
tail -10 "$D/meson-setup.log" 2>/dev/null
echo "=== ninja.log tail ==="
tail -12 "$D/ninja.log" 2>/dev/null
echo "=== errors ==="
grep -nE "error:|ERROR:|FAILED" "$D/meson-setup.log" "$D/ninja.log" 2>/dev/null | tail -8
