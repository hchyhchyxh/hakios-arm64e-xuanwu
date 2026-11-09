#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/a8xx
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/msm/registers/adreno
curl -sL --max-time 40 -A curl "$KB/a6xx_gmu.xml" -o "$D/a6xx_gmu.xml" -w "%{http_code} %{size_download} a6xx_gmu.xml\n" || true
echo
grep -nE "PWR_CLK_STATUS|GMU_PWR|CX_MISC" "$D/a6xx_gmu.xml" | head -12
