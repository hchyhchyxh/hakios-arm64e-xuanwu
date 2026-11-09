#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
mkdir -p "$D"
for url in \
  "https://raw.githubusercontent.com/freedreno/registers/main/a8xx.xml" \
  "https://raw.githubusercontent.com/freedreno/registers/master/a8xx.xml" \
  "https://raw.githubusercontent.com/freedreno/registers/main/adreno/a8xx.xml" ; do
  curl -sL --max-time 40 -A curl "$url" -o "$D/a8xx.xml" -w "%{http_code} %{size_download} $url\n" || true
  sz=$(stat -c%s "$D/a8xx.xml" 2>/dev/null || echo 0)
  [ "$sz" -gt 10000 ] && break
done
echo
echo "=== power/clk status registers ==="
grep -nE "CX_MISC_GFX_PWR_CLK_STATUS|GMU_PWR_CLK_STATUS|GFX_PWR|PWR_CLK" "$D/a8xx.xml" 2>/dev/null | head -10
echo
echo "=== GDSC / collapse registers ==="
grep -nE "GDSCR|GDSC|COLLAPSE|POWER_ON" "$D/a8xx.xml" 2>/dev/null | head -10
