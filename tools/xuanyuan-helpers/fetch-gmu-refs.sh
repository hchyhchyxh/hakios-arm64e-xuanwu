#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/metal-refs/a8xx
fetch() { curl -sL --max-time 40 -A "curl" "$1" -o "$OUT/$2" -w "%{http_code} %{size_download} $2\n" || true; }
BASE=https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno
fetch "$BASE/a6xx_gmu.c" a6xx_gmu.c
fetch "$BASE/a7xx_gmu.c" a7xx_gmu.c
fetch "$BASE/a6xx_gpu.c" a6xx_gpu.c
ls -la "$OUT" | tail -8
