#!/bin/bash
set -u
D=/mnt/x/ios/work/metal-refs/a8xx
echo "=== includes in a8xx_gpu.c / a6xx_gmu.c ==="
grep -nE "#include|a8xx_regs|REG_A8XX_CX_MISC" "$D/a8xx_gpu.c" "$D/a6xx_gmu.c" 2>/dev/null | head -15
echo
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
for p in \
  "drivers/gpu/drm/msm/registers/adreno/a8xx.xml" \
  "drivers/gpu/drm/msm/adreno/a8xx.xml" \
  "drivers/gpu/drm/msm/registers/adreno/a6xx.xml" ; do
  out="$D/$(basename $(dirname $p))-$(basename $p)"
  curl -sL --max-time 40 -A curl "$KB/$p" -o "$out" -w "%{http_code} %{size_download} $p\n" || true
done
echo
echo "=== CX_MISC status in fetched xmls ==="
grep -rnE "CX_MISC_GFX_PWR_CLK_STATUS|GMU_PWR_CLK_STATUS" "$D"/*.xml 2>/dev/null | head -5
