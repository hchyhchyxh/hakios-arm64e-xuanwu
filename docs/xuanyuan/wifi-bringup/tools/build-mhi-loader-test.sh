#!/bin/bash
set -u
X=/home/hcy/hakios-arm64e-xuanyuan
ISO=$X/xnu-src/isolated/xnu-xnu-12377.121.6
O=$ISO/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
if ! grep -q "test-hwparams-mhi.py" "$X/base/tools/mk-ath12k.sh"; then
  python3 - "$X/base/tools/mk-ath12k.sh" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
old = "python3 /mnt/x/ios/scripts/early-fw-start.py || { echo \"early fw start patch failed\"; exit 1; }"
new = old + "\npython3 /mnt/x/ios/scripts/test-hwparams-mhi.py || { echo \"hwparams test patch failed\"; exit 1; }"
if "test-hwparams-mhi.py" not in s:
    s = s.replace(old, new, 1)
p.write_text(s)
print("mk hook added")
PY
fi
cd "$X" || exit 1
bash base/tools/mk-ath12k.sh > /mnt/x/ios/logs/mk-ath12k-mhiloader.log 2>&1
echo "ath12k rc=$?"
tail -3 /mnt/x/ios/logs/mk-ath12k-mhiloader.log
touch "$ISO/bsd/dev/hakios_dada_wifi.c"
rm -f "$O"/mach.development.bcm2837 "$O"/mach.development.bcm2837.unstripped
rm -rf "$O"/mach.development.bcm2837.dSYM "$O"/mach.development.bcm2837.link
bash docs/xuanyuan/tools/rebuild-verified-kernel.sh > /mnt/x/ios/logs/kernel-mhiloader.log 2>&1
echo "kernel rc=$?"
ls -la /mnt/x/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed
