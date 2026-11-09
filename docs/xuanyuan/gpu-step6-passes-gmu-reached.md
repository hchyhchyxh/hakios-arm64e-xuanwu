# GPU stage two: step 6 passes — the gen8 GMU is reached (2026-11-09 20:50)

## What worked in this run

With the GMU offsets re-anchored for the a8xx GMU base (`gmu_bias=8192`: the a8xx GMU starts
0x2000 bytes higher relative to the GPU base, from patch 10's `GMU_BYTE_OFFSET = offset<<2 -
mmio_offset`), the CX GBIF configuration from patch 15, and the RPMh rail vote:

```
adreno: 6: about to write gbif gbif qsb side0 at 0x3d0f00c: 0x71620
adreno: 6: about to write gbif qsb side1 ... side3 = 0x71620
adreno: step 6: read registers of the GMU (0x3d6a000): the first words from the GPU block itself
adreno: 6: gmu ao_gpu_cx_busy_status (0x3d8ec30) = 0x30000000
adreno: 6: gmu ... (the other registers 0)
adreno: 6: the GMU's counter counts at 0 Hz ... (firmware may start it)
adreno: step 6: done
```

Step 6 completes: the GMU window is readable at the re-anchored offsets, GBIF takes the writes, and
one AON status register reads a real nonzero value (0x30000000) - the block is mapped and answering.
The crystal counter is still 0, consistent with the GMU not being started yet (the driver does that in
steps 15-19 after the firmware is loaded).

## Where it stops

Step 7 ("read the GMU's RPMh channel") panics: it touches the redwood RSCC block at 0x3de0000, which
is not the sm8750 location (the sm8750 dtsi has no such RSCC node; gen8 uses a different
RPMh-sequencer shape).  Next fix: skip/re-target the RSCC step for xuanyuan (its TCS state is not
needed before the GMU firmware path) or find the sm8750 equivalent from the device tree.

## Next actions

1. Re-target/skip step 7 (RSCC) behind a boot arg, rerun, and let steps 8-10 run (SMMU probe,
   SMMU context, GMU memory self-test - all at sm8750-known addresses: smmu 0x3da0000+0x40000).
2. Then steps 11-15: TrustZone zap signature with `gen80000_zap.mbn` (needs the a8xx zap load path),
   GMU space, GMU firmware write (`gen80000_gmu.bin`), start.
3. The reference patches are in `docs/xuanyuan/reference/` (a8xx-patch10/11/15).  The crystal counter
   (19.2 MHz) is the check that the GMU is genuinely up.

All runs RAM-only; loader returns cleanly after each (this one with a panic, recoverable).
