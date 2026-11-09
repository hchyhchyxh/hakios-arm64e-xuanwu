#!/bin/bash
D=/mnt/x/ios/work/metal-refs/sm8750
echo "=== gcc-sm8750.c: GPU branch definitions ==="
for n in gcc_gpu_cfg_ahb_clk gcc_gpu_memnoc_gfx_clk gcc_gpu_snoc_dvm_gfx_clk gcc_ddrss_gpu_axi_clk gcc_gpu_gpll0_clk_src gcc_gpu_gpll0_div_clk_src; do
  echo "--- $n ---"
  grep -n -A6 "$n = {" "$D/gcc-sm8750.c" 2>/dev/null | head -10
done
echo
echo "=== gpucc-sm8750.c: GPUCC registers ==="
for n in gpu_cc_gx_gdscr gpu_cc_cx_gdscr gpu_cc_gx_gmu_clk gpu_cc_cx_gmu_clk gpu_cc_ahb_clk gpu_cc_cxo_aon_clk gpu_cc_gmu_clk_src gpu_cc_cx_hw_ctrl gpu_cc_gx_domain_addr gpu_cc_gx_gmu_cbcr; do
  echo "--- $n ---"
  grep -n -A8 "$n = {" "$D/gpucc-sm8750.c" 2>/dev/null | head -12
done
