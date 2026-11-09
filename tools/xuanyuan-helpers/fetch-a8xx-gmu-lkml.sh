#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
URL="https://lore.kernel.org/linux-arm-msm/20250930-a8xx-gmu-v1-0-000000000000@kernel.org/raw"
# try the known thread page and the raw patch series from lore, then grep GMU offsets
for u in \
  "https://lore.kernel.org/linux-arm-kernel/cp7djnezyp4whhfqcnsfpes5kxfbyvqvc2ceimdrnrl7s7agyk@z7ozx6oihezd/raw" \
  "https://lore.kernel.org/linux-arm-kernel/cp7djnezyp4whhfqcnsfpes5kxfbyvqvc2ceimdrnrl7s7agyk@z7ozx6oihezd/" ; do
  code=$(curl -sL --max-time 40 -A curl -o "$D/lkml-a8xx.html" -w "%{http_code}" "$u")
  echo "$code $(stat -c%s "$D/lkml-a8xx.html") $u"
  [ "$code" = "200" ] && grep -q "GMU_" "$D/lkml-a8xx.html" && break
done
echo
echo "=== GMU offsets found in the patch series ==="
grep -oE 'offset="0x[0-9a-f]+" name="GMU_[A-Z0-9_]+"' "$D/lkml-a8xx.html" 2>/dev/null | head -20
grep -oE 'GMU_(CM3_SYSRESET|CM3_BOOT_CONFIG|CM3_FW_INIT_RESULT|ALWAYS_ON_COUNTER[A-Z_]*|HFI_[A-Z_]+|SYS_BUS_CONFIG)[^<]{0,40}' "$D/lkml-a8xx.html" 2>/dev/null | head -20
