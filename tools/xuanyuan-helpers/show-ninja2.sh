#!/bin/bash
D=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
tail -25 "$D/ninja.log" 2>/dev/null
echo "=== errors ==="
grep -nE "error:|FAILED|unknown target|no rule" "$D/ninja.log" 2>/dev/null | tail -10
