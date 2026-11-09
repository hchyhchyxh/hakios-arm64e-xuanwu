#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-steps-3-4-5-passed.md docs/xuanyuan/gpu-steps-3-4-5-passed.md
git add docs/xuanyuan/gpu-steps-3-4-5-passed.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Pass GPU steps 3, 4 and 5 on the Adreno 830

With the sm8750 GPUCC/GCC offsets and three probe-only leniency flags (adreno_voted for VOTED CBCR
status bits, adreno_nostop so the a660 stale_stop is not run on gen8, clock_on not failing on the
unreliable status bit), the driver reaches step 5: GPUCC registers read sane values, the CX GDSC
collapse bit is cleared, the GMU clock source is set to the 500 MHz tap, all GPUCC branches are
enabled and the a660 debug-clock measurement is tolerated.  Step 6's gate reads the CX GMU CBCR
status bit and needs the same leniency; after that come the first GMU reads and steps 7-10.
MSG
git log --oneline -1
