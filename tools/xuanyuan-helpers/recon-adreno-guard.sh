#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
echo "=== the guard ==="
grep -n "THE GPU IS NOT STARTED" "$K"
N=$(grep -n "THE GPU IS NOT STARTED" "$K" | head -1 | cut -d: -f1)
sed -n "$((N-40)),$((N+15))p" "$K"
echo
echo "=== firmware paths / names in driver ==="
grep -nE 'a660_|fw/|firmware|gmu.bin|sqe.fw|aqe.fw|zap' "$K" | head -30
echo
echo "=== step 10 and neighbours ==="
grep -nE "step 10:|step 11:|step_gmu|step_.*\(" "$K" | sed -n '1,40p'
