#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/pcie-hardware-and-plan.md docs/xuanyuan/pcie-hardware-and-plan.md
git add docs/xuanyuan/pcie-hardware-and-plan.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Record the PCIe hardware the Wi-Fi path needs

peach Wi-Fi is a PCIe device and this tree has no PCIe host controller at all (no sources, no conf
entries), which is why enabling hakios_dada_wifi.c fails to link.  The upstream sm8750 dtsi gives the
whole pcie0 map (parf 0x1c00000, dbi 0x40000000, elbi, atu, config 0x40100000, mhi 0x1c03000, IRQs
140-150, 2 lanes, qmp gen3x2 PHY); Linux's pcie-qcom.c is fetched as the reference.  The note gives
the bring-up order (PHY/clocks/resets -> RC/link training -> config space -> MSI -> ath12k) and what
can be checked before Wi-Fi: register reads and link-up, then peach enumeration.
MSG
git log --oneline -1
