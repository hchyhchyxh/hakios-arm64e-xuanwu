#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export https_proxy=$http_proxy
D=/home/hcy/dada-upstream
if [ ! -d "$D/.git" ]; then
  rm -rf "$D"
  git clone --depth 1 --filter=blob:none --sparse https://github.com/superboyyy/hakios-arm64e-dada.git "$D" 2>&1 | tail -3
fi
cd "$D" || exit 1
git sparse-checkout set gpu2 userland/tools userland/userland/src userland/userland/build.sh kernel 2>&1 | tail -2
echo "=== gpu2 ==="
ls -la gpu2 2>/dev/null | head -20
echo "=== userland tools (metal-related) ==="
ls userland/tools 2>/dev/null | head -20
echo "=== rw_null_air reference anywhere ==="
grep -rn "rw_null_air" --include=*.sh --include=*.py --include=*.mk --include=build.sh . 2>/dev/null | head -8
