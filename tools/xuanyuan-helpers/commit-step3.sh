#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-step3-passed-step4-cx-status.md docs/xuanyuan/gpu-step3-passed-step4-cx-status.md
git add docs/xuanyuan/gpu-step3-passed-step4-cx-status.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Retarget the GPUCC/GCC offsets to sm8750; step 3 passes, step 4 hits the gen8 CX status

The GPUCC window now uses the sm8750 offsets (0x9xxx) and the GCC branches the 0x71008/0x71010/
0x71150 ones; with adreno_voted=1 (the AHB branch is BRANCH_HALT_VOTED on this SoC) step 3 completes
and every GPUCC register reads a sane value, CX GMU/CXO clocks included.  Step 4 clears the CX GDSC
collapse bit but its a6xx power-on bit does not reflect the gen8 state: a6xx.xml marks the a6xx
CX_MISC_GFX_PWR_CLK_STATUS/GMU_PWR_CLK_STATUS as A8XX-, and gen8 uses CX_MISC_SLICE_ENABLE_FINAL.
Next: accept the cleared collapse bit for the probe (adreno_voted) to reach steps 5-10, and fetch the
A8XX register definitions for an exact cx_is_on().
MSG
git log --oneline -1
