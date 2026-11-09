#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
# 1. replace tools/mesa with the HakiOS mainline set
rm -rf tools/mesa
mkdir -p tools/mesa/air tools/mesa/redwood-drm
cp /mnt/x/ios/work/hakios-tools-mesa/air/* tools/mesa/air/
cp /mnt/x/ios/work/hakios-tools-mesa/hakios-drm/* tools/mesa/redwood-drm/ 2>/dev/null || true
cp /mnt/x/ios/work/hakios-tools-mesa/*.patch tools/mesa/
echo "air files: $(ls tools/mesa/air | wc -l), patches: $(ls tools/mesa/*.patch | wc -l)"
# 2. make mk-mesa's patch hash list match the patches that exist
python3 - <<'PY'
import re
p="tools/mk-mesa.sh"
s=open(p).read()
want = ["air-frontend.patch","air-ir3.patch","freedreno-darwin.patch","ir3-round-default.patch","osmesa-es-profile.patch"]
expr = ' '.join('"$ROOT/tools/mesa/%s"' % w for w in want)
s = re.sub(r'cat "\$ROOT"/tools/mesa/[^|]*\| sha256sum > out/built-from-air', 'cat %s | sha256sum > out/built-from-air' % expr, s, count=1)
s = re.sub(r'cat "\$ROOT"/tools/mesa/\*\.patch[^|]*\| sha256sum > out/built-from', 'cat %s | sha256sum > out/built-from' % expr, s, count=1)
open(p,"w").write(s)
print("mk-mesa patch list updated")
PY
grep -n "sha256sum > out/built-from" tools/mk-mesa.sh | head -3
# 3. clean rebuild
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
rm -rf xnu-deps/build/mesa/mesa-25.0.7 xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
LOG=/mnt/x/ios/logs/air-hakios2.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -12 "$LOG"
ls -la xnu-deps/build/mesa/out 2>/dev/null | head -10
