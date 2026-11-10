#!/bin/bash
set -u
X=/home/hcy/hakios-arm64e-xuanyuan
ISO=$X/xnu-src/isolated/xnu-xnu-12377.121.6
O=$ISO/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
echo "=== before: link products ==="
ls -la "$O"/mach.* 2>/dev/null | head -5
rm -f "$O"/mach.development.bcm2837* "$O"/mach.*.unstripped
bash "$X/docs/xuanyuan/tools/rebuild-verified-kernel.sh" > /mnt/x/ios/logs/kernel-forcelink.log 2>&1
echo "rc=$?"
grep -iE "error:" /mnt/x/ios/logs/kernel-forcelink.log | head -5
U=$O/mach.development.bcm2837.unstripped
echo "=== after: strings in kernel ==="
strings "$U" 2>/dev/null | grep -c "nw core:"
strings "$U" 2>/dev/null | grep -c "nw qmi event:"
ls -la "$U" /mnt/x/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed
sha256sum /mnt/x/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed
