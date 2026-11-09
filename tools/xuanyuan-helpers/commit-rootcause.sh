#!/bin/bash
# Copy the root-cause note into the repo and commit it
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
cp /mnt/x/ios/work/lockscreen-clock-root-cause.md docs/xuanyuan/lockscreen-clock-root-cause.md
git add docs/xuanyuan/lockscreen-clock-root-cause.md userland/tools/mesa/air/nir_air.h
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -m "Record the lock-screen clock's glass path and the two renderer options

The clock digits are the SDF backdrop composite (CABackdropLayer + CAFilter + CASDFLayer) that
the poster's vibrant title material selects; the software renderer cannot composite it, which is
why the digits seen on a black background disappear once the poster exists.  The note lists the
public K90 fix (GPU renderer), the port's own GL software renderer path (GL=1, Mesa), and the
fallback of forcing a non-material title content style at the CoverSheet consumer.
Also restore the missing AIR frontend header nir_air.h that the userland tree lost, so Mesa and
the AIR library can build again." 2>&1 | tail -3
echo "=== git log ==="
git log --oneline -3
