#!/bin/bash
B=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
ls -d "$B"/mesa-25.2.8 "$B"/build-softpipe-freedreno 2>/dev/null
echo "=== meson-setup tail ==="
tail -12 "$B"/meson-setup.log 2>/dev/null
echo "=== ninja tail ==="
tail -8 "$B"/ninja.log 2>/dev/null
