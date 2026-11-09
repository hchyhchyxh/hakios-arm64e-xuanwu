# Lock-screen clock: final state of this round (2026-11-09)

## What is verified working

`ios26-clock-t5.bin` (payload in `X:/air6/xiaomi15ultra-hakios/phone-ios26-test/`) - white large time digits on the lock screen, verified twice on the device (`ios26-clock-t5-20261109/screen1.png`, `ios26-clock-t5-verify-20261109/screen1.png`):

```
cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0
xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1
dadabuttons=1 buttons_steps=5
caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 clockplain=1 clocka11y=1 sb_wallpaper=0
```

Mechanism: `sb_wallpaper=0` keeps the lock screen in the poster-less state, whose clock is drawn as an ordinary label; `clockplain=1` swaps the title content style to opaque colours and `clocka11y=1` turns Reduce Transparency on, which turns the untinted pink SDF mask into white digits. Background is black because no poster is drawn.

## What is blocked, and why

- **Clock + poster/wallpaper together**: with a poster the clock is the `SwiftUI.SDFLayer` / `CASDFLayer` glass composite; neither CoreAnimation's software renderer nor the port's GL renderer (`eagl.c` + Mesa) draws `CASDFLayer`. This needs the Metal renderer (the K90 route: RedwoodMetal + metal2vulkan/Turnip on the Adreno). The port's GPU work is at stage 1 only (power/clock), so this is not a userspace fix.
- **Backlight / display wake**: `backboardd` logs `no backlight found`; the kernel display driver has no brightness/backlight and no DSI wake sequence, so a screen sleep is unrecoverable until reboot. Added `displaykeep=1` as a guard in libredwood26 (drops the lock button HID events and pushes `SBAutoLockTime` out); it is compiled in and its logic is exercised only when the power key is pressed, which has not been done on the device yet.
- **Gradient background attempt**: a plain `CAGradientLayer` behind the cover sheet was tried twice. The first implementation swizzled `-[CSCoverSheetViewController viewDidLoad]`, but that method is inherited from `UIViewController`, so the swizzle replaced it globally and broke the UI (the clock vanished again). That swizzle is reverted; the gradient is not wired in the current build. A safe implementation must insert the layer from a delayed block without touching inherited methods.

## Files

| Path | What |
|---|---|
| `phone-ios26-test/ios26-clock-t5.bin` | the verified working clock payload |
| `phone-ios26-test/ios26-clock-final.bin` | latest build (clockgrad reverted to a log line, displaykeep compiled) - not the verified clock image |
| `X:/ios/work/lockscreen-clock-sdf-fallback.md` | the experiment matrix and the correction |
| `X:/ios/work/lockscreen-clock-final.md` | this note |
