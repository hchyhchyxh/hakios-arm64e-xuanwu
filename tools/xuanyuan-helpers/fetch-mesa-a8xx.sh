#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
M=https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/registers/adreno
try() {
  local f="$1"
  code=$(curl -sL --max-time 50 -A curl "$M/$f" -o "$D/mesa-$f" -w "%{http_code}")
  echo "$code $(stat -c%s "$D/mesa-$f" 2>/dev/null || echo 0) $f"
}
try a8xx.xml
try a8xx_gmu.xml
try a6xx_gmu.xml
echo
echo "=== GMU CM3/ALWAYS_ON entries in fetched files ==="
for f in "$D"/mesa-a8xx.xml "$D"/mesa-a8xx_gmu.xml "$D"/mesa-a6xx_gmu.xml; do
  [ -s "$f" ] || continue
  echo "--- $(basename $f) ---"
  grep -nE 'name="GMU_(CM3_SYSRESET|CM3_BOOT_CONFIG|CM3_FW_INIT_RESULT|ALWAYS_ON_COUNTER[A-Z_]*|HFI_CTRL_STATUS|HFI_VERSION_INFO|SYS_BUS_CONFIG)"' "$f" | head -12
done
