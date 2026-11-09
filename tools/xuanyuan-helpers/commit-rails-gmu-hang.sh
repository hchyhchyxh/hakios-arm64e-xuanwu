#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/gpu-rails-acked-gmu-bus-hang.md docs/xuanyuan/gpu-rails-acked-gmu-bus-hang.md
git add docs/xuanyuan/gpu-rails-acked-gmu-bus-hang.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Rails ACKED, GMU bus hangs at offset 0: the AHB clock is the real gate

The RPMh rail vote works (RSC 0x16520000 id 0x30800 v3, gfx/cx/mx at highest corners, batch ACKED)
and the CX GDSC retrigger runs; but the first read of the GMU window at offset 0 (0x3d6c000) hangs
the kernel while the a660 offsets inside the same window had read zeros.  The GPU block's clocks are
therefore not actually running: gcc_qmip_gpu_ahb_clk is QMP-managed on sm8750 and needs the QMP vote
(the Wi-Fi driver in this tree has the QMP code), after which the GDSC/clock sequence and the gen8
GMU register map can be tried for real.
MSG
git log --oneline -1
