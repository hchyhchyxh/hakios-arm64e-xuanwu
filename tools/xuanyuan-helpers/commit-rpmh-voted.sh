#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-rpmh-voted-gmu-zero.md docs/xuanyuan/gpu-rpmh-voted-gmu-zero.md
git add docs/xuanyuan/gpu-rpmh-voted-gmu-zero.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Vote the GPU's RPMh rails on the Adreno 830 (first real power vote)

Reuse the touch driver's RSC/TCS code in redwood_adreno.c: step 4 now looks up gfx.lvl/cx.lvl/mx.lvl
in the command DB (already parsed by step 1) and writes a three-command TCS batch through the
application processor's RSC at 0x16520000 (id 0x30800, version 3).  The batch is ACKED and the rails
go to their highest listed corners.  The GMU window still reads all zero afterwards, which is either
an unclocked block (QMP vote / GDSC retrigger) or a660 GMU offsets not being gen8's; the note lists
the sweep and re-trigger that separate the two.
MSG
git log --oneline -1
