#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-step6-passes-gmu-reached.md docs/xuanyuan/gpu-step6-passes-gmu-reached.md
git add docs/xuanyuan/gpu-step6-passes-gmu-reached.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
GPU step 6 passes on the Adreno 830: GBIF programmed, GMU window answering

Re-anchor the GMU offsets for the a8xx GMU base (gmu_bias=0x2000, from patch 10's
GMU_BYTE_OFFSET = offset<<2 - mmio_offset), program CX GBIF from patch 15, and step 6 completes:
the GMU window is readable, every register returns a value (mostly 0, the AON busy status reads
0x30000000), and the crystal counter is 0 as expected before the GMU is started.  Step 7 then panics
on the redwood RSCC block at 0x3de0000, which is not where gen8 has it; the next step is to
skip/re-target that step and continue with steps 8-10 (SMMU, SMMU context, GMU memory), then the
gen80000 zap/firmware path.
MSG
git log --oneline -1
