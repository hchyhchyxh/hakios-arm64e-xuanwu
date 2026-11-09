# Adreno 8xx GMU register map, recovered from the LKML series (2026-11-09 20:35)

Source: the `[PATCH 00/17] drm/msm/adreno: Introduce Adreno 8xx family support` review thread,
archived at `X:/ios/work/metal-refs/a8xx/thread.mbox` (18,756 lines, fetched from lore.kernel.org).
It contains the a8xx register XML entries (the rebase GMU offsets commit) and the a8x GMU support
patches.  These are the offsets the driver's GMU steps must use instead of the a660 ones.

## gen8 GMU registers (word offsets into the GMU region)

| Register | Offset (words) | Notes |
|---|---|---|
| `GMU_ICACHE_CONFIG` | 0x1f400 | new on a8xx |
| `GMU_DCACHE_CONFIG` | 0x1f401 | |
| `GMU_SYS_BUS_CONFIG` | 0x1f40f | |
| `GMU_MRC_GBIF_QOS_CTRL` | 0x1f50b | |
| `GMU_PWR_CLK_STATUS` | 0x1f7e8 | a6xx-a7xx variant only; gen8 has its own status (see below) |
| `GPU_GMU_CX_GMU_RPMH_POWER_STATE` | 0x1f7e9 | a6xx-a7xx |
| `GMU_CM3_SYSRESET` | 0x1f800 | |
| `GMU_CM3_BOOT_CONFIG` | 0x1f801 | |
| `GMU_CM3_FW_BUSY` | 0x1f81a | |
| `GMU_CM3_FW_INIT_RESULT` | 0x1f81c | 0xbabeface when firmware runs |
| `GMU_CM3_CFG` | 0x1f82d | |
| `GMU_CX_GMU_POWER_COUNTER_ENABLE` | 0x1fc10 (a8xx) | gen8-specific counter block |
| `GMU_CX_GMU_POWER_COUNTER_XOCLK_0_L/H` | 0x1fca0/0x1fca1 | gen8 counter pair |
| `GMU_TCM_RET_CNTL` etc. | 0x0039 region | CX misc |

The a8xx entries marked `variants="A8XX"` (positive) in the thread are the gen8-specific ones:
`GMU_CX_GMU_POWER_COUNTER_ENABLE` at 0x1fc10, `CX_MISC_SLICE_ENABLE_FINAL` at CX_MISC 0x0087, etc.
The `A8XX-` entries in upstream `a6xx_gmu.xml` explicitly do **not** apply to gen8.

## What this changes in the driver

`GMU_WORD(n)` in `redwood_adreno.c` currently points at the a660 map (CM3_SYSRESET at 0x14000
bytes). The gen8 map puts the CM3 block at 0x1f800 words - the GMU window from the sm8750 dtsi is
0x3d6c000+0x32000, and 0x1f800 words = 0x7e000 bytes does not fit, so the a8xx offsets must be
scaled/checked against the *vendor* GMU register base (the same XML has a base for the GMU block in
the vendor dt; that constant has to come from the a8xx dtb of the phone).  The mbox has the patches
that show how Linux computes it (`a6xx_gmu.c` with the a8xx branch, `REG_A8XX_*` macros).

## Next steps

1. Extract from the mbox the exact `a8xx.xml`/`a6xx_gmu.xml` diff hunks and the
   `a6xx_gmu.c`/`a8xx_gpu.c` a8xx code paths (the GMU offset rebase commit) into a local reference.
2. Re-anchor `GMU_*` offsets and the GMU window size (0x32000) to the a8xx map; keep the a660 values
   under a default so the redwood path stays valid.
3. Then the QMP vote for `gcc_qmip_gpu_ahb_clk`, the GDSC retrigger, and the GMU window sweep:
   the crystal counter (19.2 MHz) is the check.
4. Continue with steps 7-15: new offsets for the GMU reset/HFI start, SMMU context, TrustZone zap
   (`gen80000_zap.mbn`), GMU firmware (`gen80000_gmu.bin`), start, and only then the Metal half.

Everything so far is RAM-only; the loader is idle and the phone is recoverable.
