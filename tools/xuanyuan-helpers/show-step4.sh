#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
N=$(grep -n "^step_cx_power(void)" "$K" | cut -d: -f1)
echo "step_cx_power at $N"
sed -n "$((N)),$((N+80))p" "$K"
