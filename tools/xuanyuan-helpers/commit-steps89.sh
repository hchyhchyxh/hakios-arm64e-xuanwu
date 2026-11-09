#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-steps8-9-passed-smmu.md docs/xuanyuan/gpu-steps8-9-passed-smmu.md
git add docs/xuanyuan/gpu-steps8-9-passed-smmu.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Pass GPU steps 8 and 9: the Adreno 830 SMMU answers with real IDs

Skipping the fuses and the redwood RSCC steps (adreno_skip=0x84) lets the driver reach the SMMU:
step 8 passes (CBARs read 0) and step 9 reads IDR0 0x4c017e18 / IDR1 0x40000015, 24 stream match
registers and 21 contexts with the context bank at 0x3dc0000 (smmu_size=0x40000).  Step 10 then
fails because the GMU's DTCM writes do not stick - the gen8 GMU power-up sequence (patch 11's
a8xx_gmu.c: CX/GMXC rails, GDSC, a8xx reset/HFI registers) has to be added before the GMU memory
self-test can pass.
MSG
git log --oneline -1
