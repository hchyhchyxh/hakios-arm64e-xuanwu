#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/metal-refs/sm8750
mkdir -p "$OUT"
fetch() { curl -sL --max-time 50 -A "curl" "$1" -o "$OUT/$2" -w "%{http_code} %{size_download} $2\n" || true; }
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/clk/qcom
fetch "$KB/gcc-sm8750.c" gcc-sm8750.c
fetch "$KB/gpucc-sm8750.c" gpucc-sm8750.c
echo
echo "=== gpu-related names in gcc-sm8750.c ==="
grep -nE "GPU_CFG_AHB|GPU_MEMNOC|GPU_SNOC|DDRSS_GPU|GPU_GPLL|GPLL0|GPU_CC" "$OUT/gcc-sm8750.c" 2>/dev/null | head -30
echo
echo "=== gpucc offsets ==="
grep -nE "PLL0_MODE|PLL1_MODE|GX_GDSCR|CX_GDSCR|GX_GMU|CX_GMU|AHB_CBCR|CXO|GMU_RCGR|CX_HW_CTRL|GX_DOMAIN|HUB" "$OUT/gpucc-sm8750.c" 2>/dev/null | head -40
