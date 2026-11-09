#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
K=xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
echo "=== 0x80860000 / cmd-db in driver ==="
grep -nE "80860000|cmd_db|cmd-db|rpmh" "$K" | head -20
echo
echo "=== kernel rebuild script ==="
sed -n '1,60p' docs/xuanyuan/tools/rebuild-verified-kernel.sh
echo
echo "=== kernel build dir / objects ==="
ls -d xnu-src/isolated/xnu-xnu-12377.121.6/BUILD/* 2>/dev/null | head -5
ls -la xnu-src/isolated/xnu-xnu-12377.121.6/BUILD/obj.before-clang21-pac/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_adreno.o 2>/dev/null || true
