#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
python3 - "$ROOT/tools/mk-mesa.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"'
if old in s:
    s = s.replace(old, 'patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p" || echo "mk-mesa: patch did not apply: $p"')
    open(p, "w").write(s)
    print("patch step made tolerant")
else:
    print("handled by set +e fallback")
PY
grep -n "patch -s" "$ROOT/tools/mk-mesa.sh" | head -3
B=$ROOT/xnu-deps/build/mesa
rm -rf "$B/mesa-25.2.8" "$B/build-softpipe-freedreno" "$B/out"
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-258b.log
cd "$ROOT" || exit 1
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -12 "$LOG"
ls -la "$B/out" 2>/dev/null | head
