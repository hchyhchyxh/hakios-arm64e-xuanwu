#!/bin/bash
set -e
M=/mnt/x/ios/work/metal-refs/a8xx/thread.mbox
R=/home/hcy/hakios-arm64e-xuanyuan/docs/xuanyuan/reference
mkdir -p "$R"
# patch 10: rebase GMU register offsets (2951..5655)
sed -n '2951,5654p' "$M" > "$R/a8xx-patch10-gmu-offsets.txt"
# patch 11: a8x GMU support (6908..7899)
sed -n '6908,7898p' "$M" > "$R/a8xx-patch11-gmu-support.txt"
# patch 15: CX GBIF config before GMU start (6131..6676)
sed -n '6131,6675p' "$M" > "$R/a8xx-patch15-cx-gbif.txt"
wc -l "$R"/a8xx-patch1*.txt
cd /home/hcy/hakios-arm64e-xuanyuan
git add docs/xuanyuan/reference
git -c user.name=hchyhchyxh -c user.email=hchyhchyxh@users.noreply.github.com commit -F - <<'MSG'
Keep the three a8xx patches the GMU bring-up needs next to the tree

Extracted from the LKML Adreno 8xx series archive (metal-refs/a8xx/thread.mbox):
- patch 10 "Rebase GMU register offsets" (the gen8 GMU offset table and what moves),
- patch 11 "Add support for A8x GMU" (the a8x GMU init/start sequence),
- patch 15 "Do CX GBIF config before GMU start" (the GBIF programming gen8 needs before the GMU).
They are the references for re-anchoring redwood_adreno.c's GMU steps and for the CX GBIF
configuration that gen8 performs before touching the GMU.
MSG
git log --oneline -1
