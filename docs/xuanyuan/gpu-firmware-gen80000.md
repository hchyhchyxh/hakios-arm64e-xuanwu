# GPU firmware for the Adreno 830 (gen80000), collected 2026-11-09

The kernel driver `bsd/dev/redwood_adreno.c` is the a660 (Adreno 660) line of this port: its steps 10-19
load `fw/a660_gmu.bin`, `fw/a660_sqe.fw`, `fw/a660_zap.mdt` and check chip ID `0x06030500`.  The
xuanyuan GPU is Adreno 830, chip ID `0x44050001`, gen80000 - so the firmware the driver needs is the
phone's own gen80000 set, and `docs/xuanyuan/gpu-bringup-status.md` names exactly these as the next
inputs.  Collected from the phone with Magisk root (`su -c cat`) since `/vendor/firmware` is not
readable to the shell user; hashes are of the bytes as they sit on the device.

| File | Source path on the phone | Bytes | SHA-256 |
|---|---|---|---|
| `gen80000_gmu.bin` | `/vendor/firmware/gen80000_gmu.bin` | 88,896 | `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5` |
| `gen80000_sqe.fw` | `/vendor/firmware/gen80000_sqe.fw` | 111,892 | `9c8f009bea112d7c06dd2ccb67e889999459e53b92f0f4859f5ea0db067ad6b5` |
| `gen80000_aqe.fw` | `/vendor/firmware/gen80000_aqe.fw` | 25,340 | `f04cb97e1e629b9003a26c5670b8f7e5b7bfeca95d35b2abd581fd8672efed20` |
| `gen80000_zap.mbn` | `/odm/firmware/gen80000_zap.mbn` | 12,088 | `7a3b490d1f818dea1479d07ee8398c249c472b9f8f0b8a12a36ae417de3acf6a` |

Copies: `X:/air6/xiaomi15ultra-hakios/gpu-fw/` (the `gpufw` empty file there is an adb artifact and
can be ignored).

Also present on the phone for reference: `gen71700_gmu.bin`, `gen70900_{aqe,sqe}.fw`,
`gen70900_zap.mbn`, `gen70e00_{sqe,zap}`, `gmu_gen70900.bin` - the earlier Adrenos, not used here.

## What the port still has to do with them

1. **Stage 4-6 of the driver for a8xx** - the GMU memory window, zap signature through TrustZone and
   the GMU firmware write are written against a6xx registers in `redwood_adreno.c`; the gen80000
   register map and HFI tables need the a8xx values (the reference is Linux msm's `a8xx_gmu.c` /
   `a8xx_gpu.c`, fetched to `metal-refs/a8xx/`).
2. **The GPU image** - `userland/build.sh` copies `fw-<name>/` into the small image (`fw/a660_*` on the
   redwood line); a `fw/` directory with the gen80000 names has to be assembled and referenced by the
   driver.
3. **After the GMU runs** - SMMU context from the driver's stage 13, the GPU command submission through
   `RedwoodGPU`'s user client (`kMsmGemSubmit`), then the Metal plug-in.  The Metal plug-in is not in
   this tree: this is the older `dada` line with a stub `redwoodmetal.m`, not `HakiosMetalCA.m` /
   `hakios-metal/` / `metal2vulkan` from the public POCO X5 Pro port.  That port's two paths and their
   build/run instructions are fetched at `metal-refs/hakios-metal-howto.md` and
   `metal-refs/hakios-metal-plan.md`; they are the reference for the userspace half.

## Reference links

- https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/reference/metal.md (Metal on the Adreno)
- https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/reference/metal-measurements.md
- https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/metal-plan.md
- https://github.com/steelbrain/metal2vulkan (Metal API + AIR -> Vulkan)
- https://github.com/UniXNU (AdrenoMetal, the Adreno 740 Metal-on-Vulkan stack)
