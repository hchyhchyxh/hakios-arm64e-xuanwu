#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
grep -nE "annibaleair|redwoodair|air_run|TARGET" "$ROOT/tools/mk-mesa.sh" | head -12
echo "=== build dirs ==="
ls "$ROOT/xnu-deps/build/mesa" | head
echo "=== cross.ini ==="
head -20 "$ROOT/xnu-deps/build/mesa/cross.ini" 2>/dev/null
