#!/bin/bash
set -u
X=/home/hcy/hakios-arm64e-xuanyuan
ISO=$X/xnu-src/isolated/xnu-xnu-12377.121.6
O=$ISO/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
# apply the qmi event log, the early firmware-start test, and the core-step logs
python3 /mnt/x/ios/scripts/core-logs-apply.py || exit 1
python3 /mnt/x/ios/scripts/qmi-event-log.py || exit 1
if ! grep -q "early-fw-start.py" "$X/base/tools/mk-ath12k.sh"; then
  python3 - "$X/base/tools/mk-ath12k.sh" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); s = p.read_text()
old = "python3 /mnt/x/ios/scripts/core-logs-apply.py || { echo \"core logs patch failed\"; exit 1; }"
new = old + "\npython3 /mnt/x/ios/scripts/early-fw-start.py || { echo \"early fw start patch failed\"; exit 1; }"
if "early-fw-start.py" not in s:
    s = s.replace(old, new, 1)
p.write_text(s)
print("mk-ath12k hook added")
PY
fi
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
cd "$X" || exit 1
bash base/tools/mk-ath12k.sh > /mnt/x/ios/logs/mk-ath12k-early.log 2>&1
echo "ath12k rc=$?"
strings base/xnu-deps/build/ath12k/out/ath12k.o 2>/dev/null | grep -c "nw: early"
rm -f "$O"/mach.development.bcm2837* "$O"/mach.*.unstripped
rm -rf "$O"/mach.development.bcm2837.dSYM "$O"/mach.development.bcm2837.link
bash docs/xuanyuan/tools/rebuild-verified-kernel.sh > /mnt/x/ios/logs/kernel-early.log 2>&1
echo "kernel rc=$?"
K=/mnt/x/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed
ls -la "$K"
U=$O/mach.development.bcm2837.unstripped
echo "=== kernel has the new logs? ==="
strings "$U" 2>/dev/null | grep -c "nw: early"
strings "$U" 2>/dev/null | grep -c "nw qmi event:"
