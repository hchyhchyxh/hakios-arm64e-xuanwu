# GPU stage two: step 1 passes on the Adreno 830, and the clock table the next steps need

## What passed (2026-11-09 19:32 run, `ios26-gpu-skip2`)

With the sm8750 block overrides as boot args (the driver patch from commit `9395fe3`), the a660-line
driver runs on the xuanyuan hardware and **step 1 completes**, reading the RPMh command DB through the
correct sm8750 window (0x81c60000):

```
adreno: overridden from boot args: cmd_db 0x81c60000 qfprom 0x221c8000 gmu 0x3d6c000+0x32000 smmu 0x3da0000+0x40000 pdc 0xb220000 tsens1 0xc229000
adreno: 1: RPMh has "gfx.lvl" at 0x30050; levels: 0 50 52 56 60 64 80 128 144 192 224 256 320 416 448 452
adreno: 1: RPMh has "cx.lvl"  at 0x30000; levels: 0 16 64 128 192 256 320 384 416 448 480
adreno: 1: RPMh has "mx.lvl"  at 0x30010; levels: 0 16 64 128 192 256 320 384 416 448
adreno: step 1: done
```

Then step 2 (fuses) panics: it reads the "error-corrected copy" at qfprom + 0x4000, which is not mapped
on sm8750 (the fused region is a single 0x221c8000 window there). Skipped with `adreno_skip=0x4`.

Step 3 then runs and stops cleanly (no panic):

```
adreno: step 3: make sure the clock of the GPU clock controller's registers runs (gcc_gpu_cfg_ahb)
adreno: 3: gcc gpu_cfg_ahb (0x171004) = 0x88000003
adreno: 3: it stays off (0x88000003): the GPU's clock controller would not answer
adreno: step 3: FAILED; the steps after it are not attempted
```

0x171004 is the redwood (sc7280) offset; on sm8750 the GPU's GCC and GPUCC registers live elsewhere.

## The sm8750 offsets (from Linux `gcc-sm8750.c`, `gpucc-sm8750.c`, kernel.org plain, fetched 19:36)

GPUCC block (driver has 0x1xxx offsets):

| clock | redwood offset (driver) | sm8750 offset |
|---|---|---|
| `gpu_cc_ahb_clk` | 0x1078 | **0x90bc** |
| `gpu_cc_gx_gmu_clk` | 0x1064 | **0x9060** |
| `gpu_cc_cx_gmu_clk` | 0x1098 | **0x90d4** |
| `gpu_cc_gmu_clk_src` | 0x1120 | **0x9318** |
| `GPU_CC_CX_GDSC` | 0x106c | **0x942c** |

GCC block:

| clock | redwood offset (driver) | sm8750 offset |
|---|---|---|
| `gcc_ddrss_gpu_axi_clk` | 0x171154 | **0x71150** |
| `gcc_gpu_gpll0_clk_src` / `_div` | 0x152000 (votes) | **0x52000** (bits 15/16) |
| `gcc_gpu_gemnoc_gfx_clk` | 0x17100c | see `gcc-sm8750.c` (branch at 0x7...; offset to be read) |
| `gcc_gpu_snoc_dvm_gfx_clk` | 0x171018 | not a branch of this GCC any more; the gen8 GPU has GBIF/SNOC inside the GPU block |
| `gpu_cfg_ahb` | 0x171004 | not present; the register-bus enable on sm8750 is `gcc_qmip_gpu_ahb_clk` |

Also new on gen8: the **GX power domain moved out of GPUCC** into `gxclkctl@0x3d64000`
(`GX_CLKCTL_GX_GDSC`, dtsi `gxclkctl: clock-controller@3d64000`); the driver has no block for it yet.

## Next actions

1. Add an `adreno_soc=sm8750` mode to the driver with the offset table above (GCC + GPUCC + GX GDSC),
   so steps 3-10 use sm8750 offsets while the redwood defaults stay as they are.
2. Read the remaining two GCC branch offsets (`gemnoc_gfx`, `qmip_gpu_ahb`) out of `gcc-sm8750.c` and
   the `gxclkctl` driver (`gxclkctl` GDSC regs) before writing the table.
3. Rebuild the kernel, rerun `adreno=10 adreno_skip=0x4` with the same overrides; iterate on steps 4-10
   (CX power, clocks, GMU first words, SMMU, GMU memory).
4. Then steps 11-15: TrustZone zap signature with the gen80000 images (already in the image at
   `/redwood/fw`), SMMU context, GMU firmware and start.

The panic at step 2's fused-copy read is harmless (RAM boot; loader returned). The fused speed bin is
not needed until the OPP/clock table steps.
