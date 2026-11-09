#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
ID='cp7djnezyp4whhfqcnsfpes5kxfbyvqvc2ceimdrnrl7s7agyk@z7ozx6oihezd'
for base in "https://lore.kernel.org/linux-arm-kernel" "https://lore.kernel.org/linux-arm-msm"; do
  code=$(curl -sL --max-time 60 -A curl "$base/$ID/t.mbox.gz" -o "$D/thread.mbox.gz" -w "%{http_code}")
  echo "$code $(stat -c%s "$D/thread.mbox.gz" 2>/dev/null || echo 0) $base/t.mbox.gz"
  if [ "$code" = "200" ] && [ -s "$D/thread.mbox.gz" ]; then
    gunzip -c "$D/thread.mbox.gz" > "$D/thread.mbox" 2>/dev/null && break
  fi
done
echo
echo "=== mbox size/lines ==="
wc -l "$D/thread.mbox" 2>/dev/null
echo "=== GMU offsets in the thread ==="
grep -oE 'offset="0x1f[0-9a-f]{3}" name="GMU_[A-Z0-9_]+"' "$D/thread.mbox" 2>/dev/null | sort -u | head -25
echo
echo "=== A8XX GMU register lines ==="
grep -oE '<reg32[^>]*GMU_[A-Z0-9_]+[^>]*>' "$D/thread.mbox" 2>/dev/null | grep -i "A8XX" | head -10
