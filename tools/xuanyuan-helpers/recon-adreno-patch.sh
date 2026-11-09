#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
K=xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
A=/mnt/x/ios/work/metal-refs/a8xx
echo "=== firmware names in Linux refs ==="
grep -rn "gen80000" "$A"/*.c 2>/dev/null | head -10
grep -rnE "\"qcom/[a-z0-9_]+\.(bin|fw|mbn|mdt)\"" "$A"/*.c 2>/dev/null | head -10
echo
echo "=== struct block definition ==="
grep -n -B3 -A14 "struct block {" "$K" | head -30
echo
echo "=== blocks declared ==="
grep -nE "^static struct block " "$K" | head -12
echo
echo "=== PE_parse_boot_argn uint64 usage examples in tree ==="
grep -rn "PE_parse_boot_argn(\"[a-z_0-9]*\", &.*, sizeof(uint64_t))" "$K" | head -5
grep -rn "uint64_t.*PE_parse_boot_argn\|PE_parse_boot_argn.*uint64" "$K" | head -5
