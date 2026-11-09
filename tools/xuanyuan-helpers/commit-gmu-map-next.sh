#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-gmu-map-and-qmp-next.md docs/xuanyuan/gpu-gmu-map-and-qmp-next.md
git add docs/xuanyuan/gpu-gmu-map-and-qmp-next.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Record the two sources the GMU bring-up still needs

The RPMh vote works and the rails are up, but the GMU window is unclocked (gcc_qmip_gpu_ahb_clk is
QMP-managed) and the a660 GMU offsets are not gen8's: a6xx_gmu.xml puts CM3_SYSRESET at 0x1f800
words while the driver uses 0x5000, and no a8xx register XML is public yet.  The vendor SM8750 kernel
source (a8xx_gmu.h/a8xx_reg.h) is the register-map source, and redwood_wifi.c already holds a full
QMP client to reuse for the GPU AHB clock vote.
MSG
git log --oneline -1
