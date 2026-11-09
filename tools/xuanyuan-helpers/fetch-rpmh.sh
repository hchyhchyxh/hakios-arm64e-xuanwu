#!/bin/bash
set -u
F=/mnt/x/ios/work/sm8750-refs/sm8750.dtsi
echo "=== rpmh/rsc nodes in sm8750.dtsi ==="
grep -nE 'rpmh|rsc@|tcs' "$F" | head -15

GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/sm8750
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
fetch() { curl -sL --max-time 40 -A curl "$1" -o "$2" -w "%{http_code} %{size_download} $(basename $2)\n" || true; }
fetch "$KB/drivers/soc/qcom/rpmh-rsc.c" "$D/rpmh-rsc.c"
fetch "$KB/drivers/soc/qcom/rpmh.c" "$D/rpmh.c"
echo
echo "=== rpmh TCS register layout (from rpmh-rsc.c) ==="
grep -nE "RSC_DRV|TCS_|CMD_|tcs_offset|drv" "$D/rpmh-rsc.c" 2>/dev/null | head -25
