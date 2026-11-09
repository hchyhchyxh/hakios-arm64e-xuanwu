#!/bin/bash
# Extract the loader-relevant facts from the decompiled xuanyuan base DTB and board overlay
OUT=/home/hcy/xuanyuan-dt
D="$OUT/base.dts"
O="$OUT/dtbo2/00.dts"
F=/mnt/x/ios/port/xuanyuan-profile/facts-extract.txt
mkdir -p "$(dirname "$F")"
{
echo "########## XUANYUAN DT FACTS (from public MiCode sources + prebuilt dtb/dtbo) ##########"
echo "=== base model / ids ==="
grep -m3 -E 'model = |qcom,msm-id|qcom,board-id' "$D"
echo
echo "=== board overlay model / ids ==="
grep -m5 -E 'model = |qcom,msm-id|qcom,board-id|xiaomi,miboard-id' "$O"
echo
echo "=== GIC ==="
sed -n '/interrupt-controller@16000000 {/,/};/p' "$D" | head -30
echo
echo "=== timer ==="
sed -n '/compatible = "arm,armv7-timer-mem"/,/};/p' "$D" | head -12
echo
echo "=== UFS ==="
sed -n '/ufshc@1d84000 {/,/};/p' "$D" | head -25
echo
echo "=== GPU ==="
sed -n '/qcom,kgsl-3d0@3d00000 {/,/};/p' "$D" | head -30
echo
echo "=== SMEM / cmd-db / ADSP reserved ==="
grep -nA4 -E 'smem_region@81d00000|aop_cmd_db_region@81c60000|adspslpi_region@9ee00000|q6_adsp_dtb_region@9ed80000|non_secure_display_region|splash_region|cnss_wlan_region|oem_vm_region@f8800000|shm@fc000000' "$D" | head -80
echo
echo "=== PSCI ==="
grep -nA8 -E '^\s*psci \{|method = ' "$D" | head -20
echo
echo "=== CPUs (MPIDR) ==="
grep -nE 'cpu@[0-9a-f]+ \{' "$D" | head -12
echo
echo "=== UART stdout ==="
grep -nE 'stdout-path|qup_uart@a9c000' "$D" | head -6
echo
echo "=== touch (overlay) ==="
sed -n '/synaptics_tcm@0 {/,/};/p' "$O" | head -40
echo
echo "=== display panel / resolution refs (overlay) ==="
grep -nE 'panel-display-resolution|max-x|max-y|display_panel|panel-avdd|mdss|dsi' "$O" | head -25
echo
echo "=== adsp remoteproc ==="
sed -n '/remoteproc-adsp@03000000 {/,/};/p' "$D" | head -20
} > "$F" 2>&1
echo "written $F ($(wc -l < "$F") lines)"
sed -n '1,60p' "$F"
