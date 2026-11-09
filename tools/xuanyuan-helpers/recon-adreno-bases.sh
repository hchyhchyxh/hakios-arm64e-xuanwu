#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
K=xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
echo "=== base address defines in driver ==="
grep -nE "^#define (GPUCC|GMU|SMMU|GPU_|CMD_DB|CORE|GX)" "$K" | head -30
echo
echo "=== gmu/gpucc base usage ==="
grep -nE "GMU_BASE|GPUCC_BASE|SMMU_BASE|0x03d9|0x3d9|0x03d6|0x3d6" "$K" | head -20
echo
echo "=== firmware names used by Linux msm for gen80000 ==="
A=/mnt/x/ios/work/metal-refs/a8xx
grep -nE "gen80000|\.fw\"|zap\.mbn|zap" "$A"/a6xx_gmu.c "$A"/a8xx_gpu.c 2>/dev/null | head -20
echo
echo "=== gpu_micro_code_mem / zap reserved memory in dtsi ==="
grep -n -B2 -A6 "gpu_micro_code_mem" /mnt/x/ios/work/sm8750-refs/sm8750.dtsi | head -30
