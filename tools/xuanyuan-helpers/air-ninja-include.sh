#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa || exit 1
M=mesa-25.0.7
# add the missing -I dirs straight into the generated compile lines (no meson reconfigure needed)
python3 - <<'PY'
p = "build-softpipe-freedreno/build.ninja"
s = open(p).read()
old = "-I../mesa-25.0.7/src/hakios-drm"
new = old + " -I../mesa-25.0.7/src/freedreno -I../mesa-25.0.7/src/freedreno/common -I../mesa-25.0.7/src/freedreno/ir3"
n = s.count(old)
s = s.replace(old, new)
open(p, "w").write(s)
print("build.ninja include lines patched:", n)
PY
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib3.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED" /tmp/lib3.log | head -6
tail -6 /tmp/lib3.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
