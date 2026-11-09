#!/bin/bash
set -u
SRC=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mesa/air
DST=/home/hcy/hakios-arm64e-xuanyuan/userland/tools/mesa/air
echo "=== upstream air dir ==="
ls "$SRC" 2>/dev/null | head -10
if [ -d "$SRC" ]; then
  cp -a "$DST" /home/hcy/tools-mesa-air-backup-$(date +%s) 2>/dev/null
  rm -rf "$DST"
  cp -a "$SRC" "$DST"
  echo "replaced: $(ls "$DST" | wc -l) files"
else
  echo "upstream air dir missing; look inside zip layout"
  find /mnt/x/ios/work/dada-repo -maxdepth 4 -type d -name air | head -5
fi
