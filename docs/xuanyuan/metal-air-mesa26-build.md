# Building the Metal shader-compiler library on Mesa 26.2.5 (2026-11-10)

The Metal plug-in (`userland/userland/src/redwoodmetal.m`) dlopens
`/redwood/lib/libredwoodair.dylib`. That library is `tools/mesa/air/air_metal.c` — the AIR
frontend plus the freedreno path it compiles Metal shaders through — built as a dylib by
`tools/mk-mesa.sh` in the HakiOS/dada line.

## Why it was hard

Two generations of the AIR frontend exist and do not fit each other:

- **HakiOS mainline** `tools/mesa/air` (15 files, no `air_metal.c`): compiles on Mesa 25.0.7.
- **dada** `tools/mesa/air` (17 files, `air_metal.c` 388 KB, `air_metal.h` 84 KB): what the
  driver in this tree (`redwoodmetal.m`, 343 KB) was written against, but it needs a Mesa
  newer than 25.0.7 (`nir_builder.fp_math_ctrl`, `air_constant_value.undefined/.more`,
  `air_linked_function.bitcode/size/entry`, `mesa_shader_stage`).
- **K90 Pro Max** `tools/mesa/air` (51 files, many `.inc`): compiles on Mesa 26.2.5, but its
  `rwa_*` API and its driver (`annibalemetal.m`) are newer than this tree's driver.

Mesa 26.2.5 is the version that both the dada AIR sources' API level and this tree's
`redwoodmetal.m` can work with. It has no `-Dosmesa` option any more; the AIR library does not
need OSMesa, so it is built with `-Dgallium-drivers=freedreno -Dopengl=false`.

## The recipe that now builds `libredwoodair.dylib` (arm64e)

Sources: `X:/ios/port/hakios-arm64e-dada/userland/tools/mesa` (dada's air + 8 patches +
`redwood-drm`) and `mesa-26.2.5.tar.xz` (SHA256 `794b3d17...`).

1. Unpack Mesa 26.2.5, copy `tools/mesa/air` to `src/compiler/air`, `tools/mesa/redwood-drm`
   to `src/redwood-drm`, apply the dada patches (nine hunks of the perf/probe patches do not
   apply to 26.2.5 and are skipped).
2. `nir_air.h` is missing from the dada air set: take the K90 one (20,252 bytes, it uses
   `mesa_shader_stage`) and extend two structs for the dada sources:
   - `struct air_constant_value`: add `bool undefined; uint32_t more[8];`
   - `struct air_linked_function`: add `const void *bitcode; size_t size; const char *entry;
     const struct air_constant_value *constants; unsigned num_constants;`
   - `struct air_resource`: add `bool unused;`
3. `redwood-drm` (the libdrm stand-in that talks to the kernel's RedGPU user client) needs the
   Mesa 26 syncobj API: add `drmSyncobjWait/Signal/TimelineSignal/TimelineWait/Query2/Transfer`
   stubs (the RedwoodGPU path does its own fencing) and their declarations to `xf86drm.h`.
4. `u_sync_provider.c`: the shim replaces libdrm, so force the DRM path:
   `#undef HAVE_LIBDRM` + `#define HAVE_LIBDRM 1` before the includes.
5. `src/gallium/drivers/freedreno/meson.build`: the `libredwoodair` target needs
   `include_directories('../../../freedreno')`, `('../../../freedreno/common')`,
   `('../../../freedreno/ir3')` and the gallium `ir3` dir, plus the IOKit/Foundation/
   CoreFoundation frameworks in `redwoodair_link_args`, and
   `link_whole : [libfreedreno_ir3, _libnir, _libmesa_util]`.
6. Cross file: `-arch arm64e -isysroot ~/theos/sdks/iPhoneOS16.5.sdk`, host machine darwin/ios/
   aarch64/arm64e. meson 1.12.1 (venv) required.
7. `ninja -C b src/gallium/drivers/freedreno/libredwoodair.dylib`; the install name is
   `-Wl,-install_name,/redwood/lib/libredwoodair.dylib` from the target.

Result: `libredwoodair.dylib`, ~3.6 MB (6.8 MB with the whole-linked NIR archives), arm64e,
install name correct, and it exports the `rwa_*` functions (`llvm-nm -gU | grep -c ' _rwa_'` = 44).

## Where it stops today

The dylib links with `-Db_lundef=false`, so it still has ~209 undefined NIR symbols
(`_nir_algebraic_impl`, `_nir_alu_instr_create`, ...). `nir_algebraic_impl` is defined in
`src/compiler/nir/nir_search.c`; the whole-linked archives did not bring the definitions in,
so on the phone dyld rejects the library with
`no compiler library: dlopen(...): symbol not found in flat namespace '_nir_algebraic_impl'`.
The next step is to make the link self-contained — link the complete NIR/util archive closure
(the way Mesa's own shared libraries do; find where the symbol is defined with
`llvm-nm -A --defined-only` over the build tree) — and re-test on the device with
`dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd` (watch for
`redwoodmetal[...]: no compiler library` disappearing).

## Build script

`docs/xuanyuan/tools/build-air-mesa26.sh` captures the whole sequence (paths assume
`X:/ios/port/hakios-arm64e-dada`, the Mesa tarball and this tree's `userland/`).
