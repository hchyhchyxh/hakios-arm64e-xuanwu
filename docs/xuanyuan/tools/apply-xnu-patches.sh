#!/bin/bash
# Apply the XNU patch series to a fresh isolated build tree.
#
# Why: the kernel that runs touch on the phone was built from a tree that also carried the GPI
# touch driver (dtouch: GPI memory-only ...).  Those changes live only as patches in
# docs/xuanyuan/patches/; a bare rebuild from the tracked source gives the older SPI-bitbang
# driver, which never identifies the chip (see docs/xuanyuan/kernel-rebuild-warning.md).
#
#   bash docs/xuanyuan/tools/apply-xnu-patches.sh [--fresh]
#
# --fresh recreates the isolated tree from xnu-src/xnu-12377.121.6 first (make-isolated-tree.sh).
set -uo pipefail
R=$(cd "$(dirname "$0")/../../.." && pwd)
P=$R/docs/xuanyuan/patches
DST=$R/xnu-src/isolated/xnu-xnu-12377.121.6

if [ "${1:-}" = "--fresh" ]; then
	bash "$R/docs/xuanyuan/tools/make-isolated-tree.sh" || exit 1
fi
[ -d "$DST" ] || { echo "no isolated tree: run with --fresh"; exit 1; }

cd "$DST" || exit 1
ok=0; fail=0; failed=""
# chronological order: the patch mtimes record the order they were written against the tree
for p in $(ls -tr "$P"/*.patch); do
	if patch -p1 -N -s --no-backup-if-mismatch < "$p" > /dev/null 2>&1; then
		ok=$((ok + 1))
	else
		fail=$((fail + 1)); failed="$failed ${p##*/}"
	fi
done
echo "patches applied: $ok, failed: $fail"
if [ $fail -gt 0 ]; then
	echo "failed:$failed"
fi

F=$DST/bsd/dev/hakios_dada_touch.c
echo "touch driver: $(stat -c%s "$F" 2>/dev/null) bytes"
missing=""
for marker in "GPI memory-only" "GPI SMMU probe" "touch transport initialized"; do
	if ! grep -q "$marker" "$F" 2>/dev/null; then
		missing="$missing \"$marker\""
	fi
done
if [ -n "$missing" ]; then
	echo "ERROR: the GPI touch driver is not in place, missing:$missing"
	exit 1
fi
echo "GPI touch driver in place (build from this isolated tree, not the bare tracked source)"
