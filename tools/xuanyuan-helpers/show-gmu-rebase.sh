#!/bin/bash
M=/mnt/x/ios/work/metal-refs/a8xx/thread.mbox
echo "=== patch subjects in mbox ==="
grep -nE '^Subject: .*(a6xx|a8xx|gmu|GMU)' "$M" | head -20
echo
echo "=== hunks around GMU rebase ==="
grep -n "Rebase GMU register offsets" "$M" | head -5
N=$(grep -n "Rebase GMU register offsets" "$M" | head -1 | cut -d: -f1)
[ -n "${N:-}" ] && sed -n "$((N)),$((N+120))p" "$M"
