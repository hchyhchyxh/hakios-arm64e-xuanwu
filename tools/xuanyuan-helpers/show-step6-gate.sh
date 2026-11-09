#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
N=$(grep -n "needs CX on" "$K" | head -1 | cut -d: -f1)
echo "gate at $N"
sed -n "$((N-25)),$((N+30))p" "$K"
