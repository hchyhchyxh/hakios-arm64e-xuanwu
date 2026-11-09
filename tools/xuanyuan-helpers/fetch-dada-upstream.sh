#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/dada-upstream
mkdir -p "$D"
R=https://raw.githubusercontent.com/superboyyy/hakios-arm64e-dada/main
fetch() { local p="$1" n="$2"; code=$(curl -sL --max-time 40 -A curl "$R/$p" -o "$D/$n" -w "%{http_code}"); echo "$code $(stat -c%s "$D/$n" 2>/dev/null || echo 0) $p"; }
fetch "README.md" README.md
fetch "xnu-src/xnu-12377.121.6/bsd/conf/files" conf-files.txt
fetch "xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_pcie.c" hakios_dada_pcie.c
fetch "xnu-src/xnu-12377.121.6/bsd/dev/hakios_pcie.c" hakios_pcie.c
fetch "xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_wifi.c" hakios_dada_wifi.c
echo
echo "=== conf files pcie entries (if fetched) ==="
grep -nE "pcie|pci" "$D/conf-files.txt" 2>/dev/null | head -10
echo
echo "=== README wifi/pcie mentions ==="
grep -n -iE "wifi|wlan|pcie|bluetooth" "$D/README.md" 2>/dev/null | head -10
