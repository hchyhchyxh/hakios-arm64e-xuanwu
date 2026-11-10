# Getting CoreAnimation onto the Metal road (2026-11-10, late)

The Metal shader-compiler library (`libredwoodair.dylib`, see
`metal-air-mesa26-build.md`) loads in backboardd and answers as Adreno 830. That was only the
first of four walls between here and the native lock-screen clock. This note records all four
and exactly where the fourth one stands.

## 1. QuartzCore's default.metallib (cleared)

CoreAnimation's Metal context aborts with `metallib_url != nil` (ogl-metal.mm:3770) when
`QuartzCore.framework/default.metallib` is missing, and the image build strips it. The file is
in the encrypted system volume of the IPSW, and `ipsw` (blacktop) decrypts AEA volumes with
keys it fetches per release:

    ipsw extract --files --pattern '.*\.metallib$' -o OUT iPhone....ipsw

271 files, 479 MB. `build.sh` now overlays them onto the system image right before mkapfs
("metallibs: overlaying ..."), which is how the phone got them (system.apfs grew to 5.58 GB).

## 2. The AIR archive slice (cleared)

`default.metallib` is an `AIR_METALLIB` binary archive whose slices are selected by
`-targetDeviceArchitecture`'s cpuType/subType. The driver answered 0 (no Apple GPU's), so
CoreAnimation logged "unable to find unknown slice or a compatible one in binary archive".
A boot arg now names the pair the archive carries:

    metal_arch=0x01000017,0xc

With it, CA logged `pipeline com.apple.coreanimation.PBGRA...: Vfx... + T...` -- it is compiling
the window server's pipelines through our driver and `libredwoodair`.

## 3. Which processes get the device (cleared)

`metal_procs=` was parsed comma-separated by the driver but space-separated by inject26.c, so
`metal_procs=backboardd,SBRendererService` matched nothing and the window server logged
"Failed to initialize Metal or OpenGLES contexts!". inject26.c now splits on commas too, and
both `backboardd` and `SBRendererService` get the device.

## 4. Surface heaps and the present path (open)

CoreAnimation allocates its surfaces in ~21 MB `MTLHeap`s. The driver's
`-newHeapWithDescriptor:` answered nil, so it logged
`[Metal] Context 1 failed to allocate 22020096-byte transient surface heap` over and over and
fell back to per-draw allocations -- the lock screen's native glass clock did appear, but the
picture tore and SpringBoard did not react to swipes.

A minimal `MTLHeap` is now in `redwoodmetal.m` (size/used tracking; buffer/texture creation
delegated to the device's own entry points; `heapBufferSizeAndAlignWithLength:options:` and
`-heapTextureSizeAndAlignWithDescriptor:` answer real numbers). With it the heap lines are gone
from the log, but the next boot shows backboardd itself aborting right after the first CA
pipelines compile:

    hakios: crash backboardd[215] signal 6 pc 2421481d0 ...
    [Metal] Metal failed to load binary archive

The pc is the same every time, so it is a deterministic assert (SIGABRT), not a memory fault.
The next step is to find that assertion (the abort message is not in the loader log; it needs
the backboardd binary at `pc - slide` or a kernel-side trace of the write to stderr) and
implement whatever Metal surface it names -- most likely in the drawable/present path
(`src/redwooddrawable.m`, which already reports `the compiler library is loaded in this process
and has rwa_texture_from_surface`) or in a texture format/sample count the window server's
pipelines use.

Until then the phone runs the stable payload (wallpaper + injected label clock + touch), and
`inject26.c` keeps CoreAnimation off Metal unless `camtl=1` is given, so a bad Metal boot cannot
take the window server down permanently.

## Boot arguments used in the experiment

    dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd,SBRendererService \
      metal_arch=0x01000017,0xc camtl=1
