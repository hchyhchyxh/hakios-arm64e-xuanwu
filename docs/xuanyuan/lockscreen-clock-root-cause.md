# Lock-screen clock: root cause and the two ways forward

The verified run (`ios26-wallpaper-retry-20261109`, boot args without `gpu=1` / `gl=1`) shows a gradient wallpaper on the lock screen and no large time digits. Earlier runs showed the digits while the background was still black; once the wallpaper appeared the digits were gone. This document records what the picture actually is made of, why the software renderer loses it, and the two public, testable ways to get it back.

## 1. What the run is drawing with

- Boot command line of the verified run: `debug=0x8 ... cpus=4 fb_threads=2 bootcpu=2 -v ... caenv=CA_DISABLE_ASYNCHRONOUS=1` — no `gpu=1`, no `gl=1`, no `metal=1`.
- `backboardd[13] [WindowServer] Failed to initialize Metal or OpenGLES contexts!` at 16:29:20. No `libredwood-gl.dylib` exists in the root image (`/redwood/lib` holds the injection libraries and `RedwoodAudio.driver` only).
- Therefore CoreAnimation draws with its own **software renderer**; `userland/src/inject26.c` says the same: "No GPU: CoreAnimation draws with its software renderer, which walks the layer tree recursively".

## 2. What the missing digits are made of

From the device dumps and the framework headers in `coversheet-objc.txt`:

- The clock container is `CSProminentTimeView`; inside it the SwiftUI hosting view `_TtGC7SwiftUI14_UIHostingViewV13CoverSheetKit14TitleLabelView_`. Both were measured on the device: 20 windows, `CSProminentTimeView` (0,0,327,144), `TitleLabelView` (41,0,245,143), `hidden = 0`, `alpha = 1.0`.
- `CoverSheetKit` has `CSCoverSheetSDFBackdropView` with `CABackdropLayer _backdropLayer`, `CAFilter _glassForegroundFilter`, `CASDFLayer _outputLayer`, `CASDFElementLayer _outputElementLayer`. The digits are not a text layer: they are an SDF mask (`CASDFElementLayer` 723x267 CGImage on the reference port) composited through a backdrop layer and a glass filter.
- Poster title style of the verified lock-screen poster: `PRPosterContentVibrantMaterialStyle` (device log: `default title role PRPosterRoleLockScreen style <PRImmutablePosterTitleStyleConfiguration: ... titleContentStyle: <PRPosterContentVibrantMaterialStyle>>`). The vibrant material style is what selects the glass/SDF path.
- The port already documents the software-renderer outcome in `userland/src/inject_sb.c`: "The clock of the lock screen is a 'vibrant' view (BSUIVibrancyEffectView in BaseBoardUI): its digits are not drawn, they are a mask ... On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's software renderer shows neither: the digits come out red, and the top of the wallpaper as it was."

So: while there is no poster yet, the lock screen draws the pre-poster clock as plain content (the digits the user saw on the black background). Once the poster is installed and its vibrant title style takes effect, the clock switches to the SDF/glass composite — `CABackdropLayer` + `CAFilter` + `CASDFLayer` — and the software renderer cannot composite that. The digits are not hidden, transparent or zero-sized; the view tree proves that. The layer that would draw them never produces pixels.

## 3. What public projects do about it

- **K90 (`iola1999/hackios-redmi-k90-annibale`)** is the only public iOS 26-on-SM8750 project whose lock screen shows the clock. `docs/analysis/2026-10-31-mercury-wallpaper.md` §8 records that the clock is drawn by backboardd's SDF glass pipeline (`glass_background_lph`, `glass_foreground_sdf_lph`, `glass_background_sdf_no_bleed_lph`), and that it only appears once the GPU path works: AIR translator support for pointers stored in local slots, attachment levels/mip views for the variable blur, unequal attachment sizes in the AIR library, and single-sample handling. Their fix is a **GPU renderer fix**, not a SpringBoard workaround.
- **`superboyyy/hakios-arm64e-dada`** (our base) has not changed since `f1ae906`; nothing newer to copy for this.
- **HakiOS upstream** (`oosexgang/hakios-xnu-kernel`) 2.3.4 commit `3b9db78` "CoreAnimation's run-time pipelines get the nearest variant" is about CoreAnimation requesting a pipeline variant its library does not hold; it matters once a GPU renderer runs, not for the software renderer.
- No public project documents a SpringBoard-side switch that makes iOS 26 draw the clock without the glass/SDF composite. The nearest thing the port already has is the user default `BSUIVibrancyEffectViewDisabled` (in `inject_sb.c`), which turns `BSUIVibrancyEffectView` content plain — the date, not the clock's SDF backdrop.

## 4. The two ways forward

1. **Give CoreAnimation a renderer that can composite the glass (first choice).** The port already has the library and the flag: `userland/userland/build.sh GL=1` builds Mesa softpipe behind `eagl.c` as `/redwood/lib/libOSMesa.8.dylib` + `libredwood-gl.dylib`, and boot arg `gl=1` makes `opengl_es()` in `inject.c` load it even without the GPU. Then `eagl.c` draws the vibrant filters — the exact thing the software renderer skips. It is slow (CPU rasteriser at 1125x2436 under a panel scaled to 1440x3200), but it answers the question "is the glass path the only thing missing" without porting the GPU.
2. **Switch the clock away from the glass style (fallback).** The title content style is consumed in SpringBoard/CoverSheet by `-[CSCoverSheetViewController updateFont:preferredTimeMaxYPortrait:preferredTimeMaxYLandscape:vibrancyConfiguration:numberingSystem:contentStyle:]` (`coversheet-objc.txt` line 7681). An opt-in hook can replace `contentStyle` with `PRPosterContentDiscreteColorsStyle` (white) at that point, i.e. after the poster is stored, unlike the earlier `clocksolid=1` experiment which only changed PosterBoard's default properties and left the stored poster on the vibrant style. If the title label honours a non-material content style on the SwiftUI path, the clock returns without the glass; this is a workaround and must be labelled as such.

## 5. Next device test

- Build `GL=1` root, boot it with `gl=1` in addition to the verified wallpaper arguments, keep the wallpaper and lock the screen. Expected log: `redwood: CoreAnimation keeps its OpenGL ES renderer` (or the `opengl_es()` failure line) and, on screen, the large time digits over the gradient.
- If the digits are still absent with GL, take the layer-tree sample of `TitleLabelView` in the same boot (the `sb_inspect_clock_layer` diagnostic that was built but never deployed) and check whether `CABackdropLayer`/`CASDFLayer` produce contents under the GL renderer at all. That decides between "glass path can work here" and "switch the style with the opt-in hook".
- Either way the phone is only RAM-booted; no partition is written.
