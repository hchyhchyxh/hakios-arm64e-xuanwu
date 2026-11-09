# Lock-screen: two more experiments (2026-11-09 19:00)

## clockgrad, safe version (no inherited swizzle)

`clockgrad_install` now runs from 70 s and 100 s delayed blocks and inserts a plain `CAGradientLayer`
view at index 0 of the first window whose class name contains `Wallpaper` or `CoverSheet`.  Run
`ios26-clock-grad-20261109` (18:52): the hooks install (`clockgrad: gradient insertion scheduled`,
`displaykeep=1: auto-lock pushed out`) but the screenshot at 170 s showed the *system* gradient with
no clock - the poster had been restored in that run, so the insertion log line never appeared (no
window matched or the timer had not fired when the log was read).  Not a fix by itself.

## clockplain with nil vibrancy

The `updateFont:...vibrancyConfiguration:...contentStyle:` hook now passes `vibrancyConfiguration = nil`
as well as the opaque discrete content style.  Run `ios26-clock-vibnox-20261109` (18:57): the white
clock draws again (6:59), but the background is black - poster-less state again.  So nil vibrancy does
not force the plain clock while a poster is present; the poster/backdrop state is what selects the SDF
clock, not the vibrancy configuration.

## The flakiness worth recording

Across otherwise identical payloads the lock screen alternates between two states: black background
with the plain white clock (good) and restored system gradient with no clock (bad).  Poster restore is
scheduled by SpringBoard's `sb_wake_posterboard`/`sb_restore_wallpaper`; with `sb_wallpaper=0` both are
skipped, yet some runs still ended with the poster restored, so something else (PosterBoard's own
boot-time seeding, or the system image's stored poster state) can activate it.  Until that is pinned
down the usable configuration needs to be checked per run by screenshot before it is called good.

## Verified good payloads

| Payload | Result |
|---|---|
| `ios26-clock-t5.bin` | white clock, black background (verified twice) |
| `ios26-clock-vibnox.bin` | white clock, black background (18:57 run) |

Both use `gl=1 clockplain=1 clocka11y=1` and the (now default) poster-less state; `sb_wallpaper=0`
in T5 makes that state the intended one.
