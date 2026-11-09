# Metal on xuanyuan: where the AIR compiler library stands (2026-11-09 22:05)

GPU and Metal device discovery are DONE on the phone (see gpu-status-gmxc.md and the run logs
2026-11-09 21:23): with `dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd` backboardd creates
a Metal device (`MetalPluginName = RedwoodMetal`, `RedwoodGPU client opened`,
`targetDeviceInfo {0,0,"Adreno830(chip0x44050001,Xiaomi15)"}`).  What is missing is the shader
compiler library `libredwoodair.dylib`, which `redwoodmetal.m` dlopens and which turns Apple's AIR
bitcode into NIR/ir3.

## The mismatch, precisely

Two open-source generations exist and they do not fit each other:

- **HakiOS mainline** (`oosexgang/hakios-xnu-kernel`): `tools/mesa/air` (15 files, includes
  `nir_air.h`) compiles against Mesa 25.0.7 — `libair.a` builds.  It has **no `air_metal.c`** and
  **no linked-functions API**.
- **dada fork** (`superboyyy/hakios-arm64e-dada`): `tools/mesa/air` has `air_metal.c` (the library
  entry points `rwa_*` that `redwoodmetal.m` calls) and a newer `nir_air.h` API
  (`air_constant_value.undefined/.more`, `mesa_shader_stage`, `air_linked_function`), but its AIR
  sources need Mesa newer than 25.0.7 (`nir_builder.fp_math_ctrl`, `nir_fp_exact`).

## What was already fixed in the local tree

`userland/tools/mesa` is now the HakiOS mainline set (air + 5 patches + `redwood-drm` copied as
`src/hakios-drm` by `tools/mk-mesa.sh`), and `air_metal.c` / `air_metal_test.c` were copied in from
the dada snapshot and patched:

- `mesa_shader_stage` -> `gl_shader_stage` (done)
- `air_constant_value.undefined` -> `supplied` (inverted; done)
- dropped the `v->more` copy for >64-bit constants (done)

Remaining compile errors (`ninja -C xnu-deps/build/mesa/build-softpipe-freedreno
src/gallium/drivers/freedreno/libredwoodair.dylib`):

1. `air_metal.c:449` function pointer: `unsigned (*)(const struct glsl_type *, bool)` passed where
   `int (*)(...)` is expected -- add a cast.
2. `air_metal.c:471` "too many arguments to function call, expected 2, have 3" -- one nir/air helper
   changed signature; drop the third argument.
3. `air_metal.c:631+` `struct air_linked_function` is incomplete -- HakiOS `nir_air.h` has no linked
   functions (dada's newer API).  Either take dada's `nir_air.h` (and the air sources that match it,
   which then need Mesa >25.0.7), or add the struct and the `air_module_*linked*` declarations to the
   local `nir_air.h` and check `nm libair.a | grep linked` for the backing symbols.

Also needed once it builds: `llvm-strip -x -o out/libredwoodair.dylib <built>`,
`install_name_tool -id /redwood/lib/libredwoodair.dylib`, then `userland/build.sh` ships it as
`root/lib/libredwoodair.dylib`.

## Environment notes that cost time

- WSL proxy: gateway `172.25.144.1`, `http://172.25.144.1:12001` works (GitHub 200).  Windows can
  also download directly and hand files over at `/mnt/x` (jsdelivr worked when api.github.com was
  rate limited).
- Run multi-line commands from `.sh` files in `X:/ios/scripts`, not inline `bash -lc '...'` inside
  PowerShell: `$VARS` in inline commands get eaten.
- `mk-mesa.sh` re-extracts Mesa when the patch hash changes, so any manual symlink/header fix inside
  `mesa-25.0.7` is lost unless added to `mk-mesa.sh`.
