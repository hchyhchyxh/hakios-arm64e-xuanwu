# GMU DTCM writes: address correct, the GMU's register clock still off (2026-11-09 20:40)

- `gmxc.lvl` is absent from xuanyuan's command DB (0 levels): no separate GMU CX rail resource.
- The re-anchored DTCM address is right: GMU_CM3_DTCM_START is XML 0x1c400 words; gen8
  (gmu base 0x3d6c000, GPU base 0x3d00000 -> mmio_offset 0x6c000) gives (0x1c400<<2)-0x6c000 =
  0x5000 -> 0x3d71000, exactly where the driver writes.
- Neither `CM3_SYSRESET` (0x3d7e000, written 1, read 0) nor DTCM words latch.  The GMU's register
  interface is therefore not clocked, not misaddressed.
- New insight from the sm8750 dtsi: the GMU has **two** power domains -
  `GPU_CC_CX_GDSC` **and** `GX_CLKCTL_GX_GDSC`, and on gen8 the GX GDSC moved out of GPUCC into the
  new `gxclkctl@0x3d64000` block.  The driver has no block for gxclkctl and never powers GX before
  the GMU steps, so the GMU's AHB side may sit in the unpowered GX domain.

## Next step

Add a `gxclkctl` block (`0x3d64000`, from the dtsi) and power the GX GDSC there (write the GDSC
control the way the driver does for CX), before step 10; then the DTCM self-test should pass and
steps 11-15 (zap, GMU firmware, start) can be attempted.  The gxclkctl register layout is in the
vendor clk driver (`gxclkctl-sm8750.c`, not in mainline yet) - fetch from a public SM8750 vendor
kernel (OnePlus/OPPO/Qualcomm GPL drop) or reuse the `GX_CLKCTL_GX_GDSC` bindings from the
`qcom,sm8750-gxclkctl` compatible in the dtsi.
