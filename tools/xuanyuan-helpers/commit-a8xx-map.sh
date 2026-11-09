#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/a8xx-gmu-register-map.md docs/xuanyuan/a8xx-gmu-register-map.md
git add docs/xuanyuan/a8xx-gmu-register-map.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Recover the gen8 GMU register map from the Adreno 8xx patch series

The LKML series "[PATCH 00/17] drm/msm/adreno: Introduce Adreno 8xx family support" carries the
register XML that is not in any public tree yet; its thread (archived locally at
metal-refs/a8xx/thread.mbox) has the rebased GMU offsets (GMU_CM3_SYSRESET 0x1f800, BOOT_CONFIG
0x1f801, FW_INIT_RESULT 0x1f81c, CM3_CFG 0x1f82d, the A8XX-positive power-counter block at 0x1fc10
and CX_MISC_SLICE_ENABLE_FINAL at CX_MISC 0x87) plus the a8x GMU support patches.  The note lists
the offsets, why the a660 GMU_WORD map is wrong for xuanyuan, and the next steps (scale against the
vendor GMU base from the phone's a8xx dtb, QMP vote, sweep, then steps 7-15).
MSG
git log --oneline -1
