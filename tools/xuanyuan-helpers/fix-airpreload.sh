#!/bin/bash
set -u
U=/home/hcy/hakios-arm64e-xuanyuan/userland/userland
python3 - "$U/src/inject26.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
anchor = '\tif ((library = dlopen(REDWOOD_GL, RTLD_NOW)) == NULL) {'
add = ('\t/* the 26-line OSMesa is built with the AIR frontend and references rwa_*; load the AIR library\n'
       '\t * first, globally, so the OpenGL library binds without a flat-namespace failure. */\n'
       '\tdlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_GLOBAL);\n')
assert anchor in s, "gl dlopen anchor not found"
s = s.replace(anchor, add + anchor, 1)
open(p, "w").write(s)
print("preloaded libredwoodair before GL")
PY
grep -n "libredwoodair.dylib" "$U/src/inject26.c" | head -3
echo "=== rebuild image ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-airpreload.log 2>&1
echo "build rc=$?"
tail -4 /mnt/x/ios/logs/root-airpreload.log
ls -la "$U/root.apfs"
echo "=== repack clock6 payload ==="
bash /mnt/x/ios/scripts/package-clock6.sh 2>&1 | tail -2
