#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
try() {
  local url="$1" out="$2"
  code=$(curl -sL --max-time 30 -A curl -o "$out" -w "%{http_code}" "$url")
  echo "$code $(stat -c%s "$out" 2>/dev/null || echo 0) $url"
  [ "$code" = "200" ] && return 0 || return 1
}
try "https://git.kernel.org/pub/scm/linux/kernel/git/drm/drm.git/plain/drivers/gpu/drm/msm/registers/adreno/a8xx.xml?h=drm-next" "$D/a8xx-drmnext.xml" || \
try "https://git.kernel.org/pub/scm/linux/kernel/git/next/linux-next.git/plain/drivers/gpu/drm/msm/registers/adreno/a8xx.xml" "$D/a8xx-linuxnext.xml" || \
try "https://raw.githubusercontent.com/freedreno/registers/main/adreno/a8xx.xml" "$D/a8xx-freedreno.xml"
echo
for f in "$D"/a8xx-drmnext.xml "$D"/a8xx-linuxnext.xml "$D"/a8xx-freedreno.xml; do
  [ -s "$f" ] && { echo "=== $f: GMU registers ==="; grep -oE 'name="GMU_[A-Z0-9_]+"|<reg32 offset="0x[0-9a-f]+" name="GMU_[A-Z0-9_]+"' "$f" | head -20; }
done
