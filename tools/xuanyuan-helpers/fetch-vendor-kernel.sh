#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/vendor-kernel
mkdir -p "$D"
RAW=https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/xuanyuan-v-oss
try() {
  local p="$1" out="$2"
  code=$(curl -sL --max-time 60 -A curl "$RAW/$p" -o "$D/$out" -w "%{http_code}")
  echo "$code $(stat -c%s "$D/$out" 2>/dev/null || echo 0) $p"
}
try "drivers/gpu/msm/adreno/a8xx_gmu.h" a8xx_gmu.h
try "drivers/gpu/msm/adreno/a8xx_reg.h" a8xx_reg.h
try "drivers/gpu/msm/adreno/a8xx_gpu.h" a8xx_gpu.h
try "drivers/gpu/msm/adreno/a8xx_catalog.c" a8xx_catalog.c
try "drivers/clk/qcom/gpucc-sm8750.c" gpucc-sm8750-vendor.c
try "drivers/clk/qcom/gcc-sm8750.c" gcc-sm8750-vendor.c
echo
echo "=== a8xx_gmu.h key registers ==="
grep -nE "CM3_SYSRESET|BOOT_CONFIG|FW_INIT_RESULT|ALWAYS_ON_COUNTER|HFI_CTRL_STATUS|HFI_VERSION|PWR_CLK_STATUS|AON" "$D/a8xx_gmu.h" 2>/dev/null | head -20
