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

## The tearing was the CPU copy, not Metal (2026-11-10)

The kernel already carries a display-controller double-buffered scanout (`hakios_dada_dpu.c`,
`hakios_dada_scanout_*`), gated behind boot arg `dadascan=1`. Default was the CPU scaling copy
of the whole frame (~9-15 ms a frame) racing the panel's scanout, which is what tore, in both
the software and Metal paths. With `dadascan=1` the log shows

    ddpu: ctl0 drives pipes 0x3 (flush 0x20003); buffers of 11534336 bytes at 0xfda00000 and 0xfe500000
    MiniIOMobileFramebuffer: the display controller shows the frames from buffers of its own

and the frame rate goes 50-60 fps with the tearing gone. Metal keeps its own two known-wrongs
(SBRendererService must stay off Metal or the lock screen is black; some layers still take the
CPU fallback), but the display path is the same for both builds.

## The build where it all runs together (2026-11-10, 21:00)

Combining the working kernel (`kernel-backup/mach.xuanyuan.working-20261109`, the GPI touch
driver), the Metal recipe and the display controller's own scanout gives one build with:

- **the native iOS 26 lock screen**: the real Liquid Glass clock (semi-transparent glass digits,
  12-hour), drawn by CoreAnimation through `libredwoodair` on the Adreno 830 -- not the injected
  label;
- **display-controller scanout** (`dadascan=1`): 51-60 fps, frames flipped from the controller's
  own double buffers, so no CPU copy races the panel;
- **real touch**: the GPI driver reports (`dtouch: touch report configuration (128 bytes)`),
  fingers reach SpringBoard;
- backboardd stays up; `SBRendererService` still aborts once (`signal 6`, the same pc every
  time) but it is not a Metal client and the window server carries on.

Boot arguments of that build:

    cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 \
      xuanyuantouch=1 dadatouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 \
      dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 clocka11y=1 \
      dadagpu=6 dadagpu_keep=1 dadascan=1 metal=1 metal_procs=backboardd \
      metal_arch=0x01000017,0xc camtl=1

Screenshots that show the clock ticking (8:57 -> 8:59) on the phone are in
`X:/air6/xiaomi15ultra-hakios/ios26-metal-final-20261110/`.

## Tearing, take two: beat-paced scaling with four copy threads (2026-11-10, 21:15)

With Metal the frames are fast enough to hit the refresh (51/s), and that exposed that the
CPU scaling copy is what tears: the display is 1125x2436 and the panel 1440x3200, so every frame
is scaled by the CPU into the panel memory, and with dadascan=1 the kernel skips the
refresh-beat wait ("if (gOnTheBeat && !fScan)") for the copy that scanout cannot take.

The fix that measured clean: no dadascan, fb_threads=4, cpus=6 -- the beat-paced path waits
for the next refresh and four threads scale the frame in ~7.3-8.6 ms, inside one refresh:

    MiniIOMobileFramebuffer: 269 frames in 5008 ms (53 a second), 8584 us each in the driver,
      after 93 us waiting for the GPU and, 263 of them, 7944 us for the next refresh

(compared with 19300 us a frame and no beat wait on the dadascan=1 build). Boot args in
docs/xuanyuan/tools/package-metal-paced.sh.
