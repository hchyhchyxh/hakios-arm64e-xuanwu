#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/sm8750-vs-redwood-constants.md docs/xuanyuan/sm8750-vs-redwood-constants.md
mkdir -p docs/xuanyuan/sm8750-refs
cp /mnt/x/ios/work/sm8750-refs/sm8750.dtsi docs/xuanyuan/sm8750-refs/sm8750.dtsi
git add docs/xuanyuan/sm8750-vs-redwood-constants.md docs/xuanyuan/sm8750-refs/sm8750.dtsi
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Take the sm8750 block addresses from the upstream device tree

Upstream Linux sm8750.dtsi (recovered from the web rather than from the phone) gives every block the
a660-line driver hardcodes: cmd-db 0x81c60000 (not 0x80860000), GMU 0x3d6c000, SMMU window 0x40000,
PDC 0xb220000, QFPROM 0x221c8000, tsens1 0xc229000, plus the gen80000 firmware names and the zap
region 0x9b09a000.  The driver now takes command-line overrides for these blocks (cmd_db, qfprom,
gmu, gmu_size, smmu_size, pdc, tsens1), defaults unchanged, so the Adreno 830 can be probed step by
step without a kernel rebuild per constant.
MSG
git log --oneline -1
