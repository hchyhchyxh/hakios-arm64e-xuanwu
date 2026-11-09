#!/bin/bash
set -u
U=/home/hcy/hakios-arm64e-xuanyuan/userland/userland
python3 - "$U/build.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \\"
new = "$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Wl,-rpath,/redwood/lib -Iobj -framework CoreFoundation \\"
assert old in s, "gl link line not found"
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("rpath added to libredwood-gl link")
PY
grep -n "rpath,/redwood/lib" "$U/build.sh" | head -2
echo "=== rebuild ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-rpath.log 2>&1
echo "build rc=$?"
tail -4 /mnt/x/ios/logs/root-rpath.log
ls -la "$U/root.apfs"
