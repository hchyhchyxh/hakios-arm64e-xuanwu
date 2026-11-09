#!/bin/bash
set -u
X1=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_touch.c
X2=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/isolated/xnu-xnu-12377.121.6/bsd/dev/hakios_dada_touch.c
python3 - "$X1" "$X2" <<'PY'
import sys
for p in sys.argv[1:]:
    s = open(p).read()
    old = "\tif (length != frame0_length || frames > 40) {\n\t\treturn;\n\t}"
    new = "\tif (length != frame0_length) {\n\t\treturn;\n\t}"
    assert old in s, "guard not found in " + p
    s = s.replace(old, new, 1)
    # rate-limit: log at most 600 differing frames, so a long touch cannot flood the log ring
    old2 = "\tif (differ == 0 && frames % 20 != 0) {\n\t\treturn;\n\t}"
    new2 = "\tif (differ == 0 || seen.frame_logs > 600) {\n\t\treturn;\n\t}\n\tseen.frame_logs++;"
    assert old2 in s, "differ guard not found in " + p
    s = s.replace(old2, new2, 1)
    # give 'seen' a counter
    old3 = "\tunsigned polls, messages, touch_reports, bad, other, touches, resyncs;"
    new3 = "\tunsigned polls, messages, touch_reports, bad, other, touches, resyncs, frame_logs;"
    assert old3 in s, "seen struct not found in " + p
    s = s.replace(old3, new3, 1)
    open(p, "w").write(s)
    print("patched", p)
PY
grep -n "frame_logs" "$X2" | head -5
echo "=== rebuild kernel ==="
bash /home/hcy/hakios-arm64e-xuanyuan/docs/xuanyuan/tools/rebuild-verified-kernel.sh > /mnt/x/ios/logs/kernel-touchdump.log 2>&1
echo "kernel rc=$?"
tail -4 /mnt/x/ios/logs/kernel-touchdump.log
ls -la /mnt/x/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed
