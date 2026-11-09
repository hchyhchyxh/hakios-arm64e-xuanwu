#!/bin/bash
# Commit the root-cause note only (userland/tools/mesa is gitignored)
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
git add docs/xuanyuan/lockscreen-clock-root-cause.md
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -m "Record the lock-screen clock's glass path and the two renderer options

The clock digits are the SDF backdrop composite (CABackdropLayer + CAFilter + CASDFLayer) that the
poster's vibrant title material selects; the software renderer cannot composite it, which is why
the digits seen on a black background disappear once the poster exists.  The note lists the public
K90 fix (GPU renderer), the port's own GL software renderer path (GL=1, Mesa), and the fallback of
forcing a non-material title content style at the CoverSheet consumer." 2>&1 | tail -3
git log --oneline -2
