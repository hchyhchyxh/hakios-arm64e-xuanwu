#!/bin/bash
# The kernel build compiles xnu-src/isolated/xnu-xnu-12377.121.6 (see
# docs/xuanyuan/tools/rebuild-verified-kernel.sh).  The repository tracks one copy of the
# source (xnu-src/xnu-12377.121.6); this script makes the isolated copy the build needs.
set -euo pipefail
R=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=$R/xnu-src/xnu-12377.121.6
DST=$R/xnu-src/isolated/xnu-xnu-12377.121.6
[ -d "$SRC" ] || { echo "$SRC is missing"; exit 1; }
mkdir -p "$R/xnu-src/isolated"
rm -rf "$DST"
cp -a "$SRC" "$DST"
echo "isolated tree in $DST ($(du -sh "$DST" | cut -f1))"
