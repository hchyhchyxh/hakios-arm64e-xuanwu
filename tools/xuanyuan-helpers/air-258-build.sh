#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
SNAP=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland
TARBALL=""
for p in "/mnt/c/Users/Administrator/Desktop/mesa-25.2.8.tar.xz" "/mnt/c/Users/Administrator/Desktop/mesa-25.2.8.tar.xz" "/mnt/c/Users/Administrator/Desktop/mesa-25.2.8.tar.xz"; do
  [ -f "$p" ] && TARBALL="$p" && break
done
if [ -z "$TARBALL" ]; then
  echo "tarball not on Desktop; searching C: user dirs"
  TARBALL=$(find /mnt/c/Users -maxdepth 3 -name "mesa-25.2.8.tar.xz" 2>/dev/null | head -1)
fi
echo "tarball: $TARBALL"
[ -n "$TARBALL" ] || exit 1
ls -la "$TARBALL"
SHA=$(sha256sum "$TARBALL" | cut -d' ' -f1)
echo "sha256: $SHA"

# use the dada generation of tools/mesa + mk-mesa (matches the air_metal.c that has linked functions)
rm -rf "$ROOT/tools/mesa"
cp -a "$SNAP/tools/mesa" "$ROOT/tools/mesa"
cp -a "$SNAP/tools/mk-mesa.sh" "$ROOT/tools/mk-mesa.sh"
echo "tools/mesa files: $(ls "$ROOT/tools/mesa" | wc -l)"

# pin 25.2.8
python3 - "$ROOT/tools/mk-mesa.sh" "$SHA" <<'PY'
import sys, re
p, sha = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r'VERSION=25\.0\.7', 'VERSION=25.2.8', s, count=1)
s = re.sub(r'SHA256=[0-9a-f]{64}', 'SHA256=' + sha, s, count=1)
open(p, "w").write(s)
print("pinned", re.search(r'VERSION=(\S+)', s).group(1))
PY

# stage the tarball where mk-mesa looks for it, wipe old tree/build
B=$ROOT/xnu-deps/build/mesa
mkdir -p "$B"
cp -f "$TARBALL" "$B/mesa-25.2.8.tar.xz"
rm -rf "$B/mesa-25.0.7" "$B/mesa-25.2.8" "$B/build-softpipe-freedreno" "$B/out"

GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-258.log
cd "$ROOT" || exit 1
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
grep -nE "error:|patch|FAILED" "$LOG" | head -10
tail -10 "$LOG"
ls -la "$B/out" 2>/dev/null | head
