#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
mkdir -p docs/xuanyuan/gpu-fw
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_gmu.bin  docs/xuanyuan/gpu-fw/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_sqe.fw   docs/xuanyuan/gpu-fw/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_aqe.fw   docs/xuanyuan/gpu-fw/
cp /mnt/x/air6/xiaomi15ultra-hakios/gpu-fw/gen80000_zap.mbn  docs/xuanyuan/gpu-fw/
cp /mnt/x/ios/work/gpu-firmware-gen80000.md docs/xuanyuan/gpu-firmware-gen80000.md
git add docs/xuanyuan/gpu-firmware-gen80000.md docs/xuanyuan/gpu-fw
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Collect the gen80000 GPU firmware and record its provenance

The xuanyuan GPU is Adreno 830 (chip 0x44050001) while the driver in this tree is the a660 line, so
the phone's own gen80000 images are the next input the bring-up status names.  Pulled with Magisk root
(/vendor/firmware is not readable to the shell), sizes and SHA-256 recorded in the note, and the Linux
msm a8xx/a6xx references fetched under metal-refs/ for the register-map port.
MSG
git log --oneline -1
