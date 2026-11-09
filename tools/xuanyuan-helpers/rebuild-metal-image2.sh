#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
U=$ROOT/userland
M=$ROOT/xnu-deps/build/mesa/out
echo "=== mesa out contents ==="
ls -la "$M" | head -12
echo "=== patch build.sh: do not auto-run mk-mesa (prebuilt libs) ==="
python3 - "$U/build.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
n1 = s.count("../tools/mk-mesa.sh")
s = s.replace("||\n\t\t\t\t../tools/mk-mesa.sh", "|| true   # prebuilt 25.0.7 libs kept")
s = s.replace("|| ../tools/mk-mesa.sh", "|| true   # prebuilt 25.0.7 libs kept")
s = s.replace("AIR=1 ../tools/mk-mesa.sh", "true")
open(p, "w").write(s)
print("mk-mesa references:", n1, "-> left:", s.count("../tools/mk-mesa.sh"))
PY
grep -n "mk-mesa.sh\|true   # prebuilt" "$U/build.sh" | head -6
echo "=== rebuild ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-metal2.log 2>&1
echo "build rc=$?"
tail -8 /mnt/x/ios/logs/root-metal2.log
ls -la "$U/root.apfs" 2>/dev/null
ls -la "$U/root/lib/libredwoodair.dylib" 2>/dev/null
