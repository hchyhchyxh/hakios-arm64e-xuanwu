# GPU stage two: status after the GMXC check (2026-11-09 20:35)

- `gmxc.lvl` is not in xuanyuan's command DB (`RPMh gmxc.lvl 0 (0 levels)`): sm8750 has no separate
  GMU CX rail resource; the GMU power comes from the existing `cx.lvl` vote + the GPUCC CX GDSC and
  the gen8 GMU power-up sequence.
- With the rails voted and the SMMU alive (steps 8/9 pass), step 10 still fails: the GMU's DTCM does
  not hold writes and the re-anchored `CM3_SYSRESET` (0x3d7e000) does not latch.  The GMU's processor
  has no power/clock yet - the a660 line never had to do gen8's power-up.

## Remaining gap (single blocker for steps 11-15)

The a8xx GMU power-up sequence from the reference patch 11 (`a8xx_gmu.c`): GMU power-on through the
CX/GMXC domain (GDSC + RPMh votes already partly in place), the a8xx reset/HFI registers, then the
boot path.  References already in the tree: `docs/xuanyuan/reference/a8xx-patch11-gmu-support.txt`,
`a8xx-patch10-gmu-offsets.txt`, `a8xx-patch15-cx-gbif.txt`; the gen80000 firmware images are in
`/redwood/fw` of every test payload; the SMMU identity (IDR0 0x4c017e18, 24 CBARs/21 contexts) is
confirmed.

## What the driver has now (all behind boot args, defaults unchanged)

`adreno_voted`, `adreno_nostop`, `adreno_rails`, `adreno_gdsc`, `adreno_gbif`, `adreno_gmsw`/`gmu_bias`,
`adreno_skip`, plus block overrides `cmd_db/qfprom/gmu/gmu_size/smmu_size/pdc/tsens/rsc`.
Steps 1, 3, 4, 5, 6, 8, 9 pass on the Adreno 830.
