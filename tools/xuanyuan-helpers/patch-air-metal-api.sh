#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
python3 - <<'PY'
p = "tools/mesa/air/air_metal.c"
s = open(p).read()
s = s.replace("mesa_shader_stage", "gl_shader_stage")
s = s.replace("v->undefined = true;", "v->supplied = false;")
s = s.replace("\t\tv->bits = words[0];\n\t\tmemcpy(v->more, &words[1], sizeof(v->more));\n\t\tv->undefined = false;",
              "\t\tv->bits = words[0];\n\t\tv->supplied = true;")
# in case the indent differs
import re
s = re.sub(r"\n\s*memcpy\(v->more, &words\[1\], sizeof\(v->more\)\);", "", s)
s = s.replace("v->undefined = false;", "v->supplied = true;")
open(p, "w").write(s)
print("patched; remaining undefined/more refs:", s.count("->undefined"), s.count("->more"))
PY
cd xnu-deps/build/mesa || exit 1
cp ../../../tools/mesa/air/air_metal.c mesa-25.0.7/src/compiler/air/air_metal.c
ninja -C build-softpipe-freedreno src/gallium/drivers/freedreno/libredwoodair.dylib > /tmp/lib6.log 2>&1
echo "ninja rc=$?"
grep -E "error:" /tmp/lib6.log | head -8
tail -5 /tmp/lib6.log
B=build-softpipe-freedreno/src/gallium/drivers/freedreno
ls -la "$B"/libredwoodair* 2>/dev/null
