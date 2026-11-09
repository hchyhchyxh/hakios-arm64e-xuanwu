#!/bin/bash
set -eu
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/dada-upstream
mkdir -p "$D"
code=$(curl -sL --max-time 300 -A curl "https://raw.githubusercontent.com/superboyyy/hakios-arm64e-dada/main/kernel/xnu-12377.121.6-dada.patch" -o "$D/xnu-dada.patch" -w "%{http_code}")
echo "http $code size $(stat -c%s "$D/xnu-dada.patch")"
echo "=== interesting diff files ==="
grep -nE "^diff --git" "$D/xnu-dada.patch" | grep -iE "pcie|wifi|ath|adreno|gpu|metal|mhi|qmp|aop" | head -30
