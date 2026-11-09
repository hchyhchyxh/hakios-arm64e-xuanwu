#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/dada-upstream
mkdir -p "$D"
R=https://raw.githubusercontent.com/superboyyy/hakios-arm64e-dada/main
fetch() { local p="$1" n="$2"; code=$(curl -sL --max-time 60 -A curl "$R/$p" -o "$D/$n" -w "%{http_code}"); echo "$code $(stat -c%s "$D/$n" 2>/dev/null || echo 0) $p"; }
fetch "kernel/apply.sh" apply.sh
fetch "kernel/README.md" kernel-README.md
fetch "gpu2/README.md" gpu2-README.md
echo
echo "=== kernel/README head ==="
head -40 "$D/kernel-README.md" 2>/dev/null
echo
echo "=== gpu2/README head ==="
head -40 "$D/gpu2-README.md" 2>/dev/null
