#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
python3 - "$ROOT/tools/mk-mesa.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'for p in "$ROOT"/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done'
new = 'for p in "$ROOT"/tools/mesa/*.patch; do patch -s -N -f --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p" || echo "mk-mesa: patch skipped: $p"; done'
if old in s:
    s = s.replace(old, new)
    open(p, "w").write(s)
    print("patch loop made non-fatal")
else:
    print("pattern not found")
PY
grep -n "patch skipped\|patch -s" "$ROOT/tools/mk-mesa.sh" | head -3
B=$ROOT/xnu-deps/build/mesa
rm -rf "$B/mesa-25.2.8" "$B/build-softpipe-freedreno" "$B/out"
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-258c.log
cd "$ROOT" || exit 1
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
grep -cE "patch skipped" "$LOG" || true
tail -10 "$LOG"
ls -la "$B/out" 2>/dev/null | head
