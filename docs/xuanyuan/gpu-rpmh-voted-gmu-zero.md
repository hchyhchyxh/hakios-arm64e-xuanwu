# GPU stage two: the RPMh rail vote works; the GMU reads still return zero (2026-11-09 20:10)

## First real power vote on the Adreno 830

Added to `redwood_adreno.c` (boot arg `adreno_rails=1`, reusing `hakios_dada_touch.c`'s TCS code):
RPMh TCS write of `gfx.lvl` / `cx.lvl` / `mx.lvl` from the command-DB entries step 1 reads.
Run `ios26-gpu-rails`:

```
adreno: 4: RPMh rails: gfx 0x30050 (16 levels) cx 0x30000 (11) mx 0x30010 (10)
adreno: 4: RPMh RSC id 0x30800 (version 3) at 0x16520000: slot status 0x1 control 0
adreno: 4: voting gfx 0x30050=0x1c4 cx 0x30000=0x1e0 mx 0x30010=0x1c0
adreno: 4: RPMh rail vote ACKED
```

So the RSC base 0x16520000 and its v3 TCS layout are right for xuanyuan, and the rails are now voted
(highest corners: 452/480/448 as the command DB lists them).  The GPU block still reads all zero at
step 6, including the GMU always-on counter:

```
adreno: 6: gmu always_on_counter_h (0x3d80224) = 0
adreno: 6: gmu always_on_counter_l (0x3d80220) = 0
```

## Two explanations to separate

1. **The block is still unclocked**: rails voted, but the GPUCC AHB/GMU clocks only start after the
   QMP vote / after a re-trigger of the clock enables once the rails are up.  (Our clock enables run
   after the vote in the same boot, but the CX GDSC write was skipped by `adreno_nostop=1`; the
   GDSC may need its collapse bit written again now that the rails are on.)
2. **The gen8 GMU register offsets are not the a660 ones**: the driver reads the a660 GMU map
   (0x5000/0x1c00 words = 0x14000/0x7000 bytes); gen8's GMU registers may live elsewhere in the
   window (the a6xx register XML shows GMU registers around 0x1f7e8..0x1f8d0 words for the newer
   layouts).  A register sweep of the GMU window would tell immediately: nonzero anywhere means the
   block is alive and the offsets are wrong; all zeros across the window means unclocked.

## Next actions

1. Add a `adreno_gmusweep=1` probe: read the GMU window at 4 KB steps (0x3d6c000..0x3d9e000) and
   print nonzero values; also read the AUX window 0x3d9e000.
2. In the same run, retry the CX GDSC sequence with the rails up (write collapse 1 then 0) instead
   of `adreno_nostop`.
3. If the sweep is all zero: QMP vote for `gcc_qmip_gpu_ahb_clk` (reuse `redwood_wifi.c`'s QMP
   message code) and then re-enable the GPUCC clocks.
4. If nonzero: re-anchor the GMU step offsets from `a6xx_gmu.xml`/gen8 register data and continue
   with steps 7-10.

RSC/TCS code is in `redwood_adreno.c` under "RPMh: the power rails".  All runs RAM-only.
