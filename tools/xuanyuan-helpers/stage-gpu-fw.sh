#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
mkdir -p userland/fw-gpu
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_gmu.bin userland/fw-gpu/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_sqe.fw  userland/fw-gpu/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_aqe.fw  userland/fw-gpu/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_zap.mbn userland/fw-gpu/
ls -la userland/fw-gpu
echo "=== gitignore check (firmware must stay local) ==="
git check-ignore -v userland/fw-gpu/gen80000_gmu.bin || echo "NOT ignored"
