# GPU stage two: rails are voted and ACKED; the GMU bus hangs at offset 0 (2026-11-09 20:15)

## Result of the sweep run

After the RPMh rail vote (`RSC id 0x30800 v3 at 0x16520000`, gfx/cx/mx at their highest listed
corners, batch ACKED) and a CX GDSC retrigger, step 6's GMU-window sweep begins:

```
adreno: 6: sweeping the GMU window 0x3d6c000..0x3d9e000 at 4 KB
[ ... kernel dies, loader restarts, no panic line in the captured log ... ]
```

The very first read of the sweep (offset 0 of the GMU window, 0x3d6c000) is what kills it.  Earlier
runs had read the **a660 offsets** inside the same window (0x3d80000, 0x3d803xx, 0x3d90xxx) and got
zeros without crashing.  So the window is partly reachable, but at least one register region inside it
needs a clock that is still off - the GMU's AHB clock is genuinely not running; the rails being voted
does not start it.

## What this means

1. **The RPMh vote path works** and is now in the driver (`adreno_rails=1`, TCS code copied from
   `hakios_dada_touch.c`): this is the first time the port has voted power rails for the GPU.
2. **The GPU block's clocks are not actually running.**  The a660-line driver enables them by CBCR
   writes, but on gen8 `gcc_qmip_gpu_ahb_clk` is QMP-managed; the missing step is the QMP vote
   (and only after it do the GPUCC AHB/GMU clocks run, which is what a register read of the GMU
   window at offset 0 needs).
3. **The a660 GMU register offsets are also suspect** (the reads that did not crash returned zeros
   where the crystal counter should count); they will need the gen8 GMU register map once the clock
   is real.

## Next steps (autonomous order)

1. **QMP vote for `gcc_qmip_gpu_ahb_clk`**: the port already has the QMP driver code in
   `bsd/dev/redwood_wifi.c` (message box at 0x0c300000, "LIAM" magic, link/channel handshake).  Add
   a small QMP client to `redwood_adreno.c` or share the Wi-Fi driver's functions; send the
   `clk_enable`/`votable` request for the GPU AHB clock and read the ack.
2. With the AHB clock running, repeat the GDSC retrigger + clock enables and re-run the sweep; then
   the GMU window should answer with the crystal counter running.
3. Fetch the gen8 GMU register map (`freedreno/registers` a8xx GMU xml or the DRM-next
   `a6xx_gmu.xml` variant set) and re-anchor step 6/7 offsets.
4. Then steps 7-15 (RPMh sequencer, SMMU, GMU memory, TrustZone zap, GMU firmware with the
   gen80000 images already in `/redwood/fw`, HFI start).

Probe aids now in the driver: `adreno_rails`, `adreno_gdsc`, `adreno_gmsw`, `adreno_voted`,
`adreno_nostop`, plus the block-address overrides (`cmd_db`, `qfprom`, `gmu`, `gmu_size`,
`smmu_size`, `pdc`, `tsens`, `rsc`).
