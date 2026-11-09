# GPU stage two: steps 1, 3, 4, 5 and the step-6 gate pass; the GMU block reads as unclocked (2026-11-09 19:55)

## Where the driver got to

Run `ios26-gpu-nostop`: `adreno=10 adreno_skip=0x4 adreno_voted=1 adreno_nostop=1` plus the sm8750
block overrides.  Steps 1, 3, 4, 5 pass, and the step-6 gate (`cx_ready`) now passes as well:

```
adreno: 6: the AHB measurement is not available on this SoC; going on (adreno_voted)
adreno: step 6: read registers of the GMU (0x3d6a000): the first words from the GPU block itself
adreno: 6: gmu cm3_sysreset (0x3d80000) = 0
adreno: 6: gmu cm3_boot_config (0x3d80004) = 0
adreno: 6: gmu cm3_fw_init_result (0x3d80070) = 0
adreno: 6: gmu hfi_ctrl_status (0x3d80600) = 0
adreno: 6: gmu hfi_version_info (0x3d80604) = 0
adreno: 6: gmu rpmh_power_state (0x3d803b0) = 0
adreno: 6: gmu sptprac_pwr_clk_status (0x3d80340) = 0
adreno: 6: gmu always_on_counter_h (0x3d80224) = 0
adreno: 6: gmu always_on_counter_l (0x3d80220) = 0
adreno: 6: the GMU's counter counts at 0 Hz: not the crystal's 19.2 MHz (firmware may start it): see the rest
```

So the GMU block window (0x3d6c000..0x3d9e000, via the override) is mapped and answers reads, but every
value is zero - including the always-on counter that must count the 19.2 MHz crystal.  The block is
therefore not clocked (or not powered) the way the driver's step 4/5 think it is.  The run ends right
after this (loader returned with code 48312, no panic line in the captured log): the GMU reads or the
next step stop the kernel silently.

## What that means: the gen8 clock/power enable path is not yet real

The a660-line driver brings CX up by clearing the GDSC collapse bit and turns the GPUCC/GCC clocks on by
plain CBCR writes.  On sm8750 that is not enough:

- every CBCR status bit reads 1 (0x88002001 / 0x8000000 style) for branches that Linux declares
  `BRANCH_HALT_VOTED`, so the writes may or may not have taken - status cannot confirm them;
- `gcc_qmip_gpu_ahb_clk` is QMP-managed on this SoC; Linux enables it through the GCC driver but the
  hardware also expects the GPUCC's CX/GMXC rails and the QMP state to be right;
- the GPUCC clocks (`cx_gmu`, `cxo`, `ahb`) only run while the CX rail (RPMh `cx.lvl` / GDSC) is
  actually up: the driver only *looked up* the RPMh levels in step 1, it never votes them.

The port already has QMP code in other drivers (`bsd/dev/redwood_wifi.c`, `redwood_adsp.c`), and the
driver already parses RPMh's command DB (`gfx.lvl` at 0x30050, `cx.lvl` at 0x30000, `mx.lvl` at
0x30010).  The missing piece is an RPMh/QMP vote path in the GPU driver:

1. RPMh vote for `gfx.lvl`/`cx.lvl`/`mx.lvl` at the levels the OPP table names (sm8750 dtsi
   `gpu_opp_table`: 160 MHz at LOW_SVS_D3 ... 1.2 GHz at TURBO_L4, levels as in the command DB), and
   GMXC if gen8 needs it - the command DB entries are already read.
2. QMP vote/ack for the QMIP GPU AHB clock (reuse redwood_wifi.c's QMP message code), or verify that
   the plain CBCR write is enough once the rails are up.
3. Only then does the GMU's always-on counter start counting, which is the check step 6 prints.
4. Then steps 7-10 (RPMh sequencer, SMMU, SMMU context, GMU memory) and 11-15 (TrustZone zap with
   `gen80000_zap.mbn`, GMU firmware `gen80000_gmu.bin`, start through HFI).

## Plan (autonomous, in order)

- **P1** RPMh/QMP vote path in `redwood_adreno.c`: reuse the command-DB entries from step 1, add a
  vote write to the RPMh TCS (the driver already has the RPMh register block addresses in its step 16
  code) - check: GMU always-on counter counts ~19.2 MHz at step 6.
- **P2** Steps 7-10 on gen8: RSCC/PDC windows (`pdc=0xb220000` already an override), SMMU
  (`smmu_size=0x40000`), SMMU context, GMU memory self-test.
- **P3** Steps 11-15: TrustZone zap signature with the gen80000 images in `/redwood/fw`, GMU space,
  GMU firmware write, start; check `GMU_CM3_FW_INIT_RESULT = 0xbabeface`.
- **P4** `adreno_keep=1` -> `RedwoodGPU` service, then the userspace Metal half (the public POCO X5
  Pro port's `metal.md` / `metal-plan.md` are the reference, fetched under `X:/ios/work/metal-refs/`).

All probe runs so far are RAM-only; nothing on the phone's storage was written.
