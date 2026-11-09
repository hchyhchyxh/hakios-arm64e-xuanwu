#!/bin/bash
D=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
echo "=== files ==="
ls -la "$D"/*.log 2>/dev/null | head
echo "=== ninja.log tail ==="
tail -35 "$D/ninja.log" 2>/dev/null
echo "=== errors ==="
grep -nE "error:|FAILED" "$D/ninja.log" 2>/dev/null | tail -12
