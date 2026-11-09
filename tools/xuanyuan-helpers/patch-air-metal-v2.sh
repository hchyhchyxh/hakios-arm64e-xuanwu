#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
cd "$ROOT" || exit 1
A=tools/mesa/air/air_metal.c
SNAP=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mesa/air/air_metal.c
echo "exists? $(ls -la "$A" 2>&1 | head -1)"
[ -f "$A" ] || cp "$SNAP" "$A"
[ -f "$A" ] || { echo "no air_metal.c anywhere"; exit 1; }
python3 - <<'PY'
p="tools/mesa/air/air_metal.c"
s=open(p).read()
s=s.replace("mesa_shader_stage","gl_shader_stage")
s=s.replace("v->undefined = true;","v->supplied = false;")
s=s.replace("v->undefined = false;","v->supplied = true;")
import re
s=re.sub(r"\n\s*memcpy\(v->more, &words\[1\], sizeof\(v->more\)\);","",s)
open(p,"w").write(s)
print("after patch: undefined=",s.count("->undefined"),"more=",s.count("->more"),"mesa_stage=",s.count("mesa_shader_stage"))
PY
cp "$A" xnu-deps/build/mesa/mesa-25.0.7/src/compiler/air/air_metal.c
ninja -C xnu-deps/build/mesa/build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib7.log 2>&1
echo "ninja rc=$?"
grep -E "error:" /tmp/lib7.log | head -8
tail -4 /tmp/lib7.log
B=xnu-deps/build/mesa/build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
