#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== xuanyuan docs with gpu/metal ==="
ls docs/xuanyuan | grep -iE "gpu|metal|graphics|render" || true
echo
echo "=== docs mentioning RedwoodGPU / metal= / turnip ==="
grep -rln "RedwoodGPU\|metal=\|turnip\|Turnip" docs/xuanyuan docs/*.md 2>/dev/null | head -10
echo
echo "=== kernel driver for gpu ==="
grep -rln "RedwoodGPU" userland/xnu xnu-src 2>/dev/null | head -10
echo
echo "=== gpu firmware dirs ==="
ls -d userland/fw-* base/fw-* 2>/dev/null || true
echo
echo "=== turnip / vulkan in tools ==="
ls userland/tools | grep -iE "turnip|vulkan|gpu" || true
grep -rn "freedreno" userland/tools/mk-mesa.sh | head -5
