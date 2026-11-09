# GPU stage two: steps 8 and 9 pass — the Adreno SMMU answers (2026-11-09 21:00)

Run `ios26-gpu-s10b`: `adreno=10 adreno_skip=0x84` (skip fuses and RSCC) with the full probe set
(rails, GDSC retrigger, CX GBIF, gmU_bias=0x2000, block overrides).

## Step 8: passed

SMMU CBARs at 0x3da1010..0x3da103c read 0, no fault; the SMMU block is reachable.

## Step 9: passed, with real hardware IDs

```
adreno: 9: smmu idr0 (0x3da0020) = 0x4c017e18
adreno: 9: smmu idr1 (0x3da0024) = 0x40000015
adreno: 9: SMMU: 24 stream match registers, 21 contexts, 32 pages of 0x1000 before the contexts' own
adreno: 9: smmu cb0 sctlr (0x3dc0000) = 0
adreno: 9: smmu cb0 ttbr0 (0x3dc0020) = 0, tcr (0x3dc0030) = 0, fsr (0x3dc0058) = 0
adreno: step 9: done
```

This is the first time the port has read the Adreno 830's SMMU identity: 24 CBARs / 21 contexts at
0x3dc0000 (the `smmu_size=0x40000` override was right).  The streaming translation stage is alive.

## Step 10: the GMU's data memory does not hold writes

```
adreno: 10: hold the GMU's processor in reset, write two words into its data memory (0x3d71000)
adreno: 10: gmu cm3_sysreset (0x3d7e000) = 0      <- write 1 did not stick
adreno: 10: gmu dtcm (0x3d71000) = 0, (0x3d71004) = 0
adreno: 10: ... write 0x600d0001 / 0xa110002, read back 0
adreno: 10: the GMU's memory does not hold what was written
adreno: step 10: FAILED
```

The writes go nowhere: the GMU's processor still has no power/clock (the a660 steps never needed the
gen8 GMU power-up), and the a8xx reset register at 0x1f800 (re-anchored 0x3d7e000) does not latch.

## Next: the gen8 GMU power-up (patch 11 reference)

`docs/xuanyuan/reference/a8xx-patch11-gmu-support.txt` shows the a8x GMU init sequence
(`a8xx_gmu.c`): GMU power-on through the CX/GMXC rails and GDSC, the a8xx reset/HFI registers
(`REG_A8XX_GPU_GMU_CX_GMU_RPMH_POWER_STATE`, `REG_A8XX_GMU_DCACHE_CONFIG`, `GMU_PWR_CLK_STATUS`
gen8 variant), then the boot/firmware path.  That sequence has to be added to `redwood_adreno.c`
before step 10 can pass; the checks it uses (the GMU's power/clk status registers) are exactly the
ones the a8xx patch reads.

Everything is RAM-only; the loader returns cleanly and the phone is recoverable.
