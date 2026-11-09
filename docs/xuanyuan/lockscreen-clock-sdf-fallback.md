# Lock-screen clock: the SDF fallback that works, and the wallpaper it costs

The lock screen now draws its large time digits on the device. They come back when CoreAnimation is told to stop using SDF layers: `caenv=CA_DISABLE_INLINE_SDFS=1` together with `caenv=CA_DISABLE_SDF=1` makes it fall back to ordinary layers, and the clock renders as plain white text. The same run's background is black: the poster's gradient does not survive the same switches. This note records the evidence, the exact boot arguments, and the open trade-off.

## 1. What the layer tree said

The bounded inspection (`sb_inspect_clock`) on the live lock screen, with the GL renderer loaded, printed the clock's layer subtree:

```
clock render layer 1 ... <SwiftUI.SDFLayer: ...>
clock render layer 3 ... filters <CABackdropLayer: ...>
clock render layer 4 ... <CASDFLayer: ...>
clock render layer 6 ... contents <__NSCFType> <CASDFElementLayer: ...>
```

Every layer: `hidden 0`, `opacity 1.000`; the `CASDFElementLayer` even carries contents. The digits are therefore not missing from the view tree - the renderer does not draw `CASDFLayer`. Neither CoreAnimation's software renderer nor the port's OpenGL ES renderer (`eagl.c` + Mesa) implements that layer class, which is why the clock stayed invisible with both.

## 2. The run that shows the clock

`ios26-clock-sdfoff.bin` (2026-11-09 17:50) - boot args:

```
cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0
xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1
dadabuttons=1 buttons_steps=5
caenv=CA_DISABLE_ASYNCHRONOUS=1 caenv=CA_DISABLE_INLINE_SDFS=1 caenv=CA_DISABLE_SDF=1
caenv=CA_DISABLE_BACKDROP_CACHE=1
gl=1 clockplain=1 clocka11y=1
```

Result: lock screen shows `5:51` in large white digits over a black background (`screen1.png` in `ios26-clock-sdfoff-20261109`). Log confirmation: `gl=1: CoreAnimation gets the OpenGL ES renderer`, `clocka11y: reduce transparency ... off`, `clockplain: CoverSheet title style hook installed`.

## 3. The switches are coupled to the wallpaper

| Run | CA switches | Clock | Wallpaper |
|---|---|---|---|
| `ios26-clock-sdfmin` | only `CA_DISABLE_INLINE_SDFS=1` (+gl) | missing | cyan-blue gradient shown |
| `ios26-clock-t2` | `CA_DISABLE_INLINE_SDFS=1`, `CA_DISABLE_SDF=1` (+gl, clockplain, clocka11y) | **shown** | black |
| `ios26-clock-sdfoff` | the two above + `CA_DISABLE_BACKDROP_CACHE=1` (+gl, clockplain, clocka11y) | **shown** | black |

So `CA_DISABLE_SDF=1` is what restores the clock and `CA_DISABLE_BACKDROP_CACHE=1` is not needed for it. The black background is caused by one of `CA_DISABLE_SDF`, `clockplain=1`, `clocka11y=1`; the next runs drop them one at a time (`clockplain` first: the earlier `clocksolid` run of the poster work had a coloured background under that hook, so `clocka11y` is the more likely cause).

## 4. What this means

- The lock screen clock is no longer blocked by the missing GPU: the CA SDF fallback draws it as ordinary text.
- The proper long-term fix is still a renderer that implements `CASDFLayer` (the Metal path the K90 port uses); the fallback is a working, clearly-labelled workaround until then.
- Remaining lock-screen work: put the gradient back with the clock (isolate the minimal switch set), then the unported display power path (backlight/wake: pressing power locks the screen and it does not light again - the kernel display driver has no backlight or DSI wake sequence).

## 5. Evidence

| Path | What |
|---|---|
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-sdfoff-20261109/screen1.png` | clock visible, black background |
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-t2-20261109/screen1.png`, `screen2.png` | clock visible, black background (backdrop cache switch removed) |
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-sdfmin-20261109/screen1.png` | gradient wallpaper, no clock |
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-plain-20261109/screen1.png` | content style made opaque: no clock (style is not the gate) |
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-a11y-20261109/screen1.png` | reduce transparency alone: no clock |
| `X:/air6/xiaomi15ultra-hakios/ios26-clock-gl3-20261109/` | GL renderer loaded; full clock layer diagnostics |
## 6. Correction (2026-11-09 18:05): the SDF switches were a confound

Three more runs separate the variables, and the earlier conclusion has to be corrected: the clock did not come back because of the SDF switches. It came back because those switches blackened the poster, and the lock screen only uses the SDF/glass clock **while a poster is active**.

| Run | Variables | Clock | Wallpaper |
|---|---|---|---|
| `ios26-clock-t3` | SDF/INLINE_SDFS switches scoped to backboardd+SpringBoard (poster extension keeps its path) | missing | gradient shown |
| `ios26-clock-t4` | **no SDF switches at all**, `sb_wallpaper=0` (poster creation skipped) | **shown** (digits pink: the untinted SDF mask) | black |
| `ios26-clock-t5` | `sb_wallpaper=0` + `clockplain=1` + `clocka11y=1` + `gl=1` | **shown, white, clean** | black |

So: with a poster the lock-screen clock is the `SwiftUI.SDFLayer`/`CASDFLayer` glass composite, and no renderer in this port draws `CASDFLayer` (software and GL both fail). Without a poster, SpringBoard draws the plain clock, which the same renderers draw fine - pink because the vibrancy colour matrix is not applied, white once `clocka11y=1` turns the transparency fallback on.

What this leaves:

1. **A usable form today**: `gl=1 clockplain=1 clocka11y=1 sb_wallpaper=0` - white large time digits on a black background. Verified: `ios26-clock-t5-20261109/screen1.png` (18:07).
2. **Clock and wallpaper together** need a renderer that implements `CASDFLayer` - i.e. the Metal path the K90 port uses (`metal=1` + RedwoodMetal + metal2vulkan/Turnip on the Adreno), which this port has not wired into the userland yet (GPU stage 1 only: power/clock bring-up).
3. Cheap cosmetic step in the meantime: draw our own plain `CAGradientLayer` behind the cover sheet (no SDF, no poster) so the lock screen is not black while the poster path waits for Metal.

The display power path (backlight/DSI wake) is still unported and is the next must-fix: pressing power locks the screen and it does not light again.
