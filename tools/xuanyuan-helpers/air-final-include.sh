#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
python3 - <<'PY'
p = "build-softpipe-freedreno/build.ninja"
s = open(p).read()
old = "-I../mesa-25.0.7/src/freedreno/ir3"
new = old + " -I../mesa-25.0.7/src/gallium/drivers/freedreno/ir3"
n = s.count(old)
s = s.replace(old, new)
open(p, "w").write(s)
print("patched:", n)
PY
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib5.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED|undefined" /tmp/lib5.log | head -8
tail -6 /tmp/lib5.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
