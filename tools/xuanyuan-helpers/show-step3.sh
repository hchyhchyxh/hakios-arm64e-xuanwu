#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
N=$(grep -n "^step_gpucc(void)" "$K" | cut -d: -f1)
echo "step_gpucc at $N"
sed -n "$((N-30)),$((N+60))p" "$K"
