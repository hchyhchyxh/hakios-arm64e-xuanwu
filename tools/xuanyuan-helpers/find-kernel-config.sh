#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
X=xnu-src/isolated/xnu-xnu-12377.121.6
echo "=== conf dir ==="
ls "$X/bsd/conf" | head -10
echo "=== config files mentioning our drivers ==="
grep -l "redwood_adreno\|hakios_dada_touch" "$X/bsd/conf"/* 2>/dev/null | head -5
echo "=== option present? ==="
grep -rn "hakios_ath12k" "$X/bsd/conf"/* 2>/dev/null | head -5
