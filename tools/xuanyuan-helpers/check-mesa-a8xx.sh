#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== local mesa registers dir ==="
ls userland/xnu-deps/build/mesa/mesa-25.0.7/src/freedreno/registers/ 2>/dev/null | head -20
echo
echo "=== a8xx files anywhere in the mesa tree ==="
find userland/xnu-deps/build/mesa -iname "a8xx*" 2>/dev/null | head -10
echo
echo "=== CM3_SYSRESET across xmls ==="
grep -rln "CM3_SYSRESET" userland/xnu-deps/build/mesa --include=*.xml 2>/dev/null | head -5
echo
echo "=== GMU register files in mesa ==="
find userland/xnu-deps/build/mesa -name "*gmu*.xml" 2>/dev/null | head -10
