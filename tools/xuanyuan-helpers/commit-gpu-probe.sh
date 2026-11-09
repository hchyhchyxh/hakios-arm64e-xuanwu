#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-stage2-first-probe.md docs/xuanyuan/gpu-stage2-first-probe.md
git add docs/xuanyuan/gpu-stage2-first-probe.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Probe the Adreno 830 with the driver's manual steps: step 1 panics at the RPMh cmd-db

The gen80000 firmware is in the small image and iOS boots with it.  With adreno=10 (the manual step
entry, not the a660-gated gpu=1) step 1 reads the GCC GPU clocks and then panics reading the
redwood-sc7280 RPMh command-db address 0x80860000, which does not exist on xuanyuan: the first
hardware constant of the a660 driver that the Adreno 830 port has to change.  The stage-one commits
and the live device-tree bases (GPU 0x03d00000, GPUCC 0x03d90000, SMMU 0x03da0000) are the baseline
to reproduce next.
MSG
git log --oneline -1
