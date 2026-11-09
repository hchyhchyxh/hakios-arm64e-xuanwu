# GPU stage two: first hardware probe on the Adreno 830 (2026-11-09 19:20)

## What ran

Image: `ios26-gpu-a10.bin` - the normal iOS 26 root + `fw/gen80000_{gmu.bin,sqe.fw,aqe.fw,zap.mbn}`
in the small image (`userland/fw-gpu/`, 246 KB), boot args:

```
adreno=10 adreno_pause=0 adreno_watch=0
```

`adreno=10` is the driver's own manual-step entry (`redwood_adreno_init`: `PE_parse_boot_argn("adreno", ...)`
runs steps 1..N; `gpu=1` instead goes through `start_by_itself()`, which requires the a660 firmware names
and the redwood SOC base and refuses on this port - that was the earlier
`THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin`).

## What it found

The firmware is in the image - the boot log lists `/redwood/fw/gen80000_gmu.bin` (88,896 bytes) - and
iOS 26 boots normally with it (home screen, touch, backboardd, all as before). So the packaging step
works.

The driver then did **step 1** on the Adreno 830 hardware and read the global clock controller:

```
adreno: step 1: read the GPU's clocks in the global clock controller
adreno: 1: gcc pll votes (0x152000) = 0x60257f
adreno: 1: gcc gpu_cfg_ahb (0x171004) = 0x88000003
adreno: 1: gcc gpu_memnoc_gfx (0x17100c) = 0x88000000
adreno: 1: gcc gpu_snoc_dvm_gfx (0x171018) = 0x80000000
adreno: 1: gcc ddrss_gpu_axi (0x171154) = 0
adreno: step 1: look up the GPU's supply rails in the boot loader's list of RPMh resources (memory at 0x80860000)
```

and there the kernel panicked:

```
== PANIC: Unhandled implementation specific error
== PANIC: Ticket spinlock[0xfffffff007f95008] timeout after 3001569 ticks; ... current owner: ...
```

0x80860000 is the **redwood (sc7280) RPMh command-db address**; on xuanyuan it is not there, so the
read hangs the bus and the kernel panics. That is the first hardware constant in the a660 driver that
the Adreno 830 needs changed, and it is exactly the class of thing the stage-two port is about.

## What this means for the port order

1. **Step 1's RPMh block** - find the Ultra's command-db address (Android's device tree, `qcom,cmd-db`
   / the `cmd_db` region under `/sys/firmware/devicetree` while in Android) and make the driver read it
   instead of 0x80860000 for this SoC.
2. **GPUCC / GMU / SMMU bases** - `docs/xuanyuan/gpu-bringup-status.md` already has them from the live
   device tree: GPU 0x03d00000, GPUCC 0x03d90000, SMMU 0x03da0000 (redwood uses different offsets in
   places), and the GMU crystal counter check is how stage one was verified.
3. **Stage-one evidence** - the git history has the earlier Ultra GPU work (`7a20d55 Verify Ultra GPU
   power clocks and crystal counter in stage one`, `9bc9632 Preserve GPU stage-one evidence and firmware
   collection gates`, `4a6f2d3 Verify stock GMU firmware readback and GPU SMMU mapping on Ultra`); the
   boot arguments and the constants used there are the baseline to reproduce before going further.
4. **Then steps 10-15** - GMU memory, zap through TrustZone, SMMU context, GMU firmware and start; the
   reference for those is Linux msm's `a6xx_gmu.c` (which now carries A8XX branches) and `a8xx_gpu.c`,
   fetched under `X:/ios/work/metal-refs/a8xx/`.
5. After a running GMU: `adreno_keep=1` starts `RedwoodGPU` for the system, which is what the Metal
   plug-in needs (`metal=1`). The Metal plug-in itself is the other half - see
   `gpu-firmware-gen80000.md` and the public POCO X5 Pro port's `metal.md`/`metal-plan.md`.

## Recovery

The panic is a normal kernel panic of a RAM-only boot: the loader returned (`result: returned 19843 0`)
and the phone can be booted to Android or the loader again. No partition was written.
