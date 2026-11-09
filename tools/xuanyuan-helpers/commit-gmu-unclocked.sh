#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-gmu-unclocked-next-rpmh-votes.md docs/xuanyuan/gpu-gmu-unclocked-next-rpmh-votes.md
git add docs/xuanyuan/gpu-gmu-unclocked-next-rpmh-votes.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Reach the GMU registers on the Adreno 830: the block answers but is unclocked

Steps 1, 3, 4, 5 and the step-6 gate pass with the sm8750 offsets and the probe leniency flags; the
GMU window (0x3d6c000) then answers every read with zero, including the always-on crystal counter,
so the GPU block is mapped but not clocked/powered the way the a660 driver assumes.  The next piece
is the RPMh/QMP vote path (the command-DB levels are already read in step 1, QMP code exists in the
Wi-Fi/ADSP drivers), then steps 7-15 with the gen80000 firmware already in the image.
MSG
git log --oneline -1
