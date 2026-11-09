#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/metal-refs/a8xx
mkdir -p "$OUT"
fetch() { curl -sL --max-time 40 -A "curl" "$1" -o "$OUT/$2" -w "%{http_code} %{size_download} $2\n" || true; }
BASE=https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno
fetch "$BASE/a8xx_gmu.c" a8xx_gmu.c
fetch "$BASE/a8xx_gpu.c" a8xx_gpu.c
fetch "$BASE/a8xx_catalog.c" a8xx_catalog.c
fetch "$BASE/a8xx.xml" a8xx.xml
fetch "$BASE/adreno_gpu.c" adreno_gpu.c
echo "=== sizes ==="
ls -la "$OUT"
