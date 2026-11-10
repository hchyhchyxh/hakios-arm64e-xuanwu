## What it took to get frames (2026-11-10, later)

The four walls above were real, and so were two more found by reading the abort text:

5. **The heap's missing `setLabel:`** -- with the minimal MTLHeap in place CoreAnimation
   labelled its surface heaps and the class answered with an unrecognized selector, which is a
   SIGABRT (`NSInvalidArgumentException`, pc 2421481d0 in the logs). `RedwoodMetalHeap` now has
   `label`/`setLabel:` (and a `dealloc`). With that, backboardd stays up and CoreAnimation's
   pipelines compile: `glass_background_sdf_no_bleed_lph`, `sdf_filter_vert_lph`,
   `narrow_blur_*`, the whole CoreAnimation set.
6. **SBRendererService must stay off Metal.** Handing the device to `SBRendererService` as well
   made the lock screen black (its layers came out black over the wallpaper). The working
   combination is `metal_procs=backboardd camtl=1`: the lock screen shows the real Liquid Glass
   clock ("7:57", semi-transparent, 12-hour) over the wallpaper.

### Working boot arguments

    cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 \
      xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 \
      buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 clocka11y=1 dadagpu=6 dadagpu_keep=1 \
      metal=1 metal_procs=backboardd metal_arch=0x01000017,0xc camtl=1

### What is still wrong

- **Tearing / ghosting**: a stale layer (e.g. the previous Settings page's "Display &
  Brightness" text behind the Search bar) and torn updates. The kernel present path already
  waits for the surfaces' GPU fences (`MiniIOSurfaceDrawn`, SWAP_GPU_MS/SWAP_NEW_MS,
  fb_queue=2), so this points at the driver's own multi-pass drawing: some passes are skipped
  or drawn by "the stand-in that draws nothing" (see the pipeline log lines), leaving old
  surface content in place; and the CPU scaling copy in the presenter still races the panel's
  scanout.
- Lock screen black only when `SBRendererService` is a Metal client; keep it out.
- `data-a-live` (Xiaomi 17 Pro, SM8850, iOS 16) solves this class of problem with
  NativeMetal + metal2vulkan + Turnip over their own kernel GPU driver; their docs note the
  same CPU-copy present path and the same "translation layer misses formats" symptom. Their
  `docs/GPU_PLAN.md` is the reference if this driver's remaining gaps get too many.
