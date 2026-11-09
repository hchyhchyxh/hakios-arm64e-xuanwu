#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
echo "=== air-ish ninja targets ==="
ninja -C build-softpipe-freedreno -t targets all 2>/dev/null | grep -iE "air" | head -20
