#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
K=xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
echo "=== 0x784000 / qfprom refs ==="
grep -n "784000\|qfprom" "$K" | head -20
echo
echo "=== step 2 function ==="
N=$(grep -n "^step_fuses(void)" "$K" | cut -d: -f1)
echo "send line $N"
sed -n "$((N-15)),$((N+40))p" "$K"
