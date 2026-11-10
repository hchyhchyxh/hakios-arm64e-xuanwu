# Kernel rebuild status: what is recovered and what is still missing (2026-11-10, 21:20)

The working kernel is `kernel-backup/mach.xuanyuan.working-20261109`
(SHA256 `922098fb...b817eb`). Two attempts to rebuild the tree failed for reasons that are now
understood, and the authoritative sources are recovered:

## Recovered: the three files that define the working kernel (docs/xuanyuan/recovered-20261109/)

`saved-progress-20261109/source-manifest.json` names exactly the tree's own changes and their
SHA256; all three are now in `docs/xuanyuan/recovered-20261109/`, hashes verified:

| file | sha256 |
|---|---|
| `bsd/dev/hakios_dada_touch.c` (the GPI touch driver, 88,356 bytes) | `8a0ba537...fa14c` |
| `bsd/dev/redwood_input.c` | `aec1cb95...894b` |
| `osfmk/console/video_console.c` | `0a74a077...d52d5` |

The handoff note in that directory states the patch series in `docs/xuanyuan/patches/` is a
**historical overlay, not to be replayed by filename order**. That is exactly what went wrong:

- the "apply every patch in mtime order" tree puts the GPI touch driver in place (89,712-byte
  driver with `GPI memory-only`, `GPI SMMU probe`, `touch transport initialized`), but the rest of
  the series does not line up with the tracked tree and the build then fails in unrelated places;
- with the series applied, the build additionally wants `bsd/dev/redwood_focaltech_fw.h`, which
  is referenced by `bsd/dev/redwood_goodix.c` but is not in the tracked tree, in `base/xnu/files`,
  or in the saved-progress copy (it was an untracked generated header in the original tree);
- `iokit/Kernel/IOUserServer.cpp` then disagrees with its generated iig stubs
  (`Panic_Impl` out-of-line definition does not match), which is the same class of drift.

## What a working rebuild needs

1. A tree equal to the one that built `mach.xuanyuan.working-20261109`: the tracked tree + the
   three recovered files, plus whatever untracked headers the original tree had
   (`redwood_focaltech_fw.h` is the one seen so far) and the generated iig stubs matching Apple's
   `IOUserServer.iig`.
2. `ath12k.o` for the peach Wi-Fi driver is **built** (`base/xnu-deps/build/ath12k/out/ath12k.o`,
   802,400 bytes, 26 `nw_wifi_*` exports) — `xnu-build.sh` links it into the kernel when present,
   which is how the Wi-Fi driver (`hakios_dada_wifi.c`, `hakios_dada_wlan.c`, both
   `optional hakios_ath12k`) gets in.
3. Only then does `dadawifi=3` have code to run. Wi-Fi is therefore **blocked on the kernel
   rebuild**, not on the chip or firmware (firmware is already in the image:
   `fw-wifi-dada/image-peach`, and firmware directories are present at 28 MB).

Until that is solved the phone runs the working kernel, and userland/display work continues there.
