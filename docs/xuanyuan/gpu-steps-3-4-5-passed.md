# GPU stage two: steps 3, 4 and 5 pass; step 6 needs the same clock-status leniency (2026-11-09 19:50)

Run `ios26-gpu-nostop` on kernel with the retargeted driver, boot args:
`adreno=10 adreno_skip=0x4 adreno_voted=1 adreno_nostop=1 cmd_db=2177236992 gmu=64405504
gmu_size=204800 smmu_size=262144 pdc=186777600 qfprom=572293120 tsens1=203591680`

## What passed

| Step | Result |
|---|---|
| 1 | RPMh cmd-db at 0x81c60000: gfx/cx/mx levels read |
| 2 | skipped (`adreno_skip=0x4`), the fused region is a different window on sm8750 |
| 3 | GPUCC register map: all 0x9xxx registers read sane values (CX GMU/CXO already running) |
| 4 | CX GDSC: collapse bit cleared, `adreno_nostop=1` leaves the earlier kernel's GPU state alone instead of the a660 stale_stop (which panics on gen8) |
| 5 | GCC votes + clocks, GMU clock source to 500 MHz (0x100602), hub source (0x100603), all GPUCC branches enabled, clock measurement (a660 debug registers) not usable on gen8 -> tolerated |

Step 5's writes are the first configuration of the Adreno 830 clock tree this port has done:

```
adreno: 5: about to write gpucc gmu_clk_src at 0x3d9931c: 0x100602
adreno: 5: about to write gpucc gmu_clk_src at 0x3d99318: 0x80000011
adreno: 5: about to write gpucc hub_clk_src at 0x3d993f0: 0x100603
adreno: 5: about to write gpucc hub_clk_src at 0x3d993ec: 0x11
adreno: 5: gpucc cx_gmu enabled (0x88002001); the status bit is not a state on this SoC
```

## Where it stops

Step 6 ("the first words" - the first GMU register reads) has its own gate:

```
adreno: 6: needs CX on (it is on) and the GMU's clock running (cx_gmu 0x88002001): steps 4 and 5
adreno: step 6: FAILED
```

It reads the CX GMU clock's CBCR and wants the status bit clear; on gen8 that bit reads 1 for every
branch (0x88002001), so the gate has to use the same leniency as `clock_on()` under
`adreno_voted=1`.  That is the next one-line change; after it, step 6 will read the GMU block
(0x3d6c000: PLL/crystal counter/versions registers) - the first real interaction with the GMU - and
steps 7-10 follow (RPMh sequencer, SMMU, SMMU context, GMU memory).

## Files

- driver: `xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c` (+ isolated copy), leniency flags:
  `adreno_voted`, `adreno_nostop`, block overrides `cmd_db/qfprom/gmu/gmu_size/smmu_size/pdc/tsens`
- payload: `phone-ios26-test/ios26-gpu-nostop.bin`
- logs: `ios26-gpu-step6-20261109/` (this run), `ios26-gpu-nostop-20261109/`, `ios26-gpu-step3-20261109/`
