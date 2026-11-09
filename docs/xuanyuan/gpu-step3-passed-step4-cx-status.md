# GPU stage two: step 3 passes, step 4 stops on the gen8 CX power status (2026-11-09 19:40)

Run `ios26-gpu-voted` (`adreno=10 adreno_skip=0x4 adreno_voted=1` + the sm8750 block overrides) on
kernel built from the retargeted driver (GPUCC offsets 0x9xxx, GCC branches 0x71008/0x71010/0x71150).

## Step 3: passed, and the GPUCC register map is real

```
adreno: 3: gcc gpu_cfg_ahb (0x171008) = 0x88000003
adreno: 3: 0x88000003: the branch is VOTED on this SoC (its off bit is not the state); going on
adreno: 3: gpucc cx_gdscr  (0x3d9942c) = 0x1
adreno: 3: gpucc pll0 mode (0x3d90000) = 0x100
adreno: 3: gpucc gmu_clk_src command (0x3d99318) = 0x80000000, source 0x100000
adreno: 3: gpucc cxo_aon  (0x3d99008) = 0x8000000
adreno: 3: gpucc gx_gmu   (0x3d99060) = 0x88000000
adreno: 3: gpucc ahb      (0x3d990bc) = 0x8000000
adreno: 3: gpucc cx_gmu   (0x3d990d4) = 0x88002001   <-- CX GMU clock is running
adreno: 3: gpucc cxo      (0x3d990e4) = 0x88000001   <-- CXO is running
adreno: 3: gpucc hub_aon  (0x3d993e8) = 0x8000000
adreno: 3: gpucc hub_cx_int (0x3d990e8) = 0x88000001
adreno: 3: gpucc smmu vote (0x3d97000) = 0x80000000
adreno: step 3: done
```

The offsets were right: the 0x9xxx GPUCC window answers, CX GMU/CXO clocks are already on (left on by
the previous kernel), the GMU clock source is at 500 MHz, the SMMU vote branch is present.

## Step 4: the write lands, the status check is a6xx's

```
adreno: 4: gpucc cx_gdscr (0x3d9942c) = 0x1
adreno: 4: about to write gpucc cx_gdscr at 0x3d9942c: 0
adreno: 4: gpucc cx_gdscr (0x3d9942c) = 0
adreno: 4: CX does not come on
adreno: step 4: FAILED; the steps after it are not attempted
```

`cx_gdscr` = 0x1 (collapse bit) goes to 0 (collapse cleared), so the write took. The check then wants
bit 31 (`GDSC_POWER_ON`) of `cx_gdscr | cx_hw_ctrl` to be set; on gen8 that bit is not the power state:

- upstream `a6xx.xml`: `CX_MISC_GFX_PWR_CLK_STATUS` is `variants="A8XX-"` (does **not** apply to A8XX),
  `GMU_PWR_CLK_STATUS` is `A8XX-` too; the A8XX-positive register there is
  `CX_MISC_SLICE_ENABLE_FINAL` (0x0087 words in CX_MISC), which `a8xx_gpu.c` writes at power-on.
- `a6xx_gmu.c`'s `a8xx_gmu_gx_is_on()` checks `REG_A8XX_CX_MISC_GFX_PWR_CLK_STATUS` and
  `REG_A8XX_GMU_PWR_CLK_STATUS` — the a8xx register database that defines those names is not in the
  upstream `adreno/*.xml` files fetched (A8XX- excludes A8XX), so it has to come from the a8xx
  register source (drm-next branch of the kernel tree, or freedreno's register database) before the
  check can be made exact.
- `cx_hw_ctrl` at the a6xx offset 0x1540 reads 0 and is not the gen8 register anyway.

## Next actions

1. For the probe: accept the cleared collapse bit as "CX switched on" under `adreno_voted=1` (with a
   line in the log), so steps 5-10 (clocks, GMU first words, SMMU, GMU memory) can run and the
   remaining offset checks surface one by one.
2. In parallel, fetch the A8XX register definitions (drm-next `drivers/gpu/drm/msm/registers/adreno/
   a8xx.xml`, or freedreno/registers) and put the exact `CX_MISC_SLICE_ENABLE_FINAL` /
   `GMU_PWR_CLK_STATUS` offsets into the driver; then `cx_is_on()` becomes real.
3. After step 5-10: steps 11-15 with the gen80000 firmware already in `/redwood/fw`
   (`gen80000_gmu.bin`, `gen80000_sqe.fw`, `gen80000_aqe.fw`, `gen80000_zap.mbn`), TrustZone zap
   signature and the SMMU context, then the GMU start.

Everything so far is RAM-only; the loader returned after the run and the phone is recoverable.
