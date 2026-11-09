#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-sm8750-step1-passed.md docs/xuanyuan/gpu-sm8750-step1-passed.md
git add docs/xuanyuan/gpu-sm8750-step1-passed.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Pass GPU step 1 on the Adreno 830 and record the sm8750 clock offsets

With the block-address overrides from the device tree, the a660-line driver's step 1 completes on
xuanyuan: the RPMh command DB reads gfx.lvl/cx.lvl/mx.lvl levels from the sm8750 window at
0x81c60000.  Step 2 (fuses) panics on the absent error-corrected copy and is skipped; step 3 then
stops at the redwood GCC offset for gpu_cfg_ahb.  The note lists the sm8750 GCC/GPUCC offsets found
in Linux gcc-sm8750.c/gpucc-sm8750.c (GPUCC 0x9xxx era) and the new gxclkctl block, which are what
the adreno_soc=sm8750 offset table has to carry next.
MSG
git log --oneline -1
