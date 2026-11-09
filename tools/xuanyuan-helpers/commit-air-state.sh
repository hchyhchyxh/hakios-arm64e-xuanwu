#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/metal-air-final-state.md docs/xuanyuan/metal-air-final-state.md
git add docs/xuanyuan/metal-air-final-state.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Record where the AIR compiler library stands

GPU bring-up and Metal device discovery are done on the phone with the dada GPU driver
(dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd): backboardd creates the Metal device and
reports Adreno830.  The one missing piece is libredwoodair.dylib, the AIR -> NIR/ir3 compiler.
The note states the two open-source generations that do not fit (HakiOS mainline air compiles on
Mesa 25.0.7 but has no air_metal.c; the dada fork has air_metal.c but needs a newer Mesa API),
what has already been patched, the exact remaining compile errors with lines, and the environment
traps (proxy gateway, shell quoting, mk-mesa re-extraction) that made this take longer than it
should.
MSG
git log --oneline -1
