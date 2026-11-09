#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/lockscreen-clock-sdf-fallback.md docs/xuanyuan/lockscreen-clock-sdf-fallback.md
git add docs/xuanyuan/lockscreen-clock-sdf-fallback.md userland/userland/src/inject26.c
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -m "Draw the lock-screen clock through CoreAnimation's SDF fallback

The clock's layer tree (SwiftUI.SDFLayer over CABackdropLayer/CASDFLayer) is present, opaque and
sized, but neither the software renderer nor the GL renderer draws CASDFLayer.  With
CA_DISABLE_INLINE_SDFS=1 and CA_DISABLE_SDF=1 CoreAnimation falls back to ordinary layers and the
digits draw; the same run loses the poster gradient.

Also give libredwood26 the GL renderer loader inject.c had (gl=1: dlopen libredwood-gl), an opt-in
clockplain=1 title-style replacement at CSCoverSheetViewController, and an opt-in clocka11y=1
reduce-transparency/liquid-glass-off switch, all with the device evidence in the new note." 2>&1 | tail -3
git log --oneline -3
