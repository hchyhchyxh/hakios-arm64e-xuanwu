#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
N=$(grep -n "^step_clocks(void)" "$K" | cut -d: -f1)
echo "step_clocks at $N"
sed -n "$((N)),$((N+55))p" "$K"
