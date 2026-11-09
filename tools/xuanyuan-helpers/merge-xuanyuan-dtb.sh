#!/bin/bash
# Merge xuanyuan base DTB + board overlay into one DTB usable by the loader/UEFI
set -e
P=/mnt/x/ios/port/xuanyuan-profile
DT=/home/hcy/xuanyuan-dt
mkdir -p "$P/dt"
echo "=== fdtoverlay available? ==="
command -v fdtoverlay || { echo "fdtoverlay missing"; exit 1; }
echo "=== merge ==="
fdtoverlay -i /mnt/x/ios/port/kernel-xuanyuan-prebuilt/dtb.img -o "$P/dt/xuanyuan-merged.dtb" "$DT/dtbo2/00.dtb"
ls -la "$P/dt/xuanyuan-merged.dtb"
echo "=== decompile merged ==="
dtc -I dtb -O dts "$P/dt/xuanyuan-merged.dtb" -o "$P/dt/xuanyuan-merged.dts" 2>/dev/null || true
wc -l "$P/dt/xuanyuan-merged.dts" 2>/dev/null || true
echo "=== merged: board ids / model ==="
grep -m5 -E 'model = |qcom,board-id|miboard-id' "$P/dt/xuanyuan-merged.dts" 2>/dev/null || true
echo "=== merged: touch / panel / gic / ufs / gpu presence ==="
grep -cE 'synaptics,tcm-spi' "$P/dt/xuanyuan-merged.dts" 2>/dev/null || true
grep -m3 -E 'mdss-dsi-panel-name|mdss-dsi-panel-width|mdss-dsi-panel-height|mdss-dsi-panel-framerate' "$P/dt/xuanyuan-merged.dts" 2>/dev/null || true
echo "=== done ==="
