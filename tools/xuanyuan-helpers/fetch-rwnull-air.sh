#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/dada-upstream/air
mkdir -p "$D"
R=https://raw.githubusercontent.com/superboyyy/hakios-arm64e-dada/main
for p in \
  "userland/userland/src/rw_null_air.h" \
  "userland/src/rw_null_air.h" \
  "userland/userland/src/rw_null.air.h" \
  "userland/userland/tools/air/rw_null_air.h" ; do
  code=$(curl -sL --max-time 40 -A curl "$R/$p" -o "$D/$(basename $p)" -w "%{http_code}")
  echo "$code $(stat -c%s "$D/$(basename $p)" 2>/dev/null || echo 0) $p"
  [ "$code" = "200" ] && break
done
echo
echo "=== files ==="
ls -la "$D" | head
