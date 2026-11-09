#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/sm8750-refs
mkdir -p "$OUT"
fetch() { curl -sL --max-time 40 -A "curl" "$1" -o "$OUT/$2" -w "%{http_code} %{size_download} $2\n" || true; }
BASE=https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/qcom
fetch "$BASE/sm8750.dtsi" sm8750.dtsi
fetch "$BASE/sm8750-mtp.dts" sm8750-mtp.dts
fetch "$BASE/sm8750-qrd.dts" sm8750-qrd.dts
ls -la "$OUT"
F="$OUT/sm8750.dtsi"
if [ -s "$F" ]; then
  echo
  echo "=== cmd-db ==="; grep -n -A6 "cmd-db\|cmd_db" "$F" | head -30
  echo
  echo "=== gpu / gmu / smmu nodes ==="; grep -nE "gpu@|gmu@|gpucc@|smmu@|adreno|zap|qcom,pas-id|firmware-name" "$F" | head -40
  echo
  echo "=== reserved memory top addresses ==="; grep -nE "reg = <0x[0-9a-f]+ 0x[0-9a-f]+>;" "$F" | head -20
fi
