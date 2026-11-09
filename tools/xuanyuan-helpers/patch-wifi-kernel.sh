#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan
for T in xnu-src/xnu-12377.121.6 xnu-src/isolated/xnu-xnu-12377.121.6; do
  F="$T/bsd/conf/files"
  sed -i 's#bsd/dev/hakios_dada_wifi.c\([[:space:]]*\)optional hakios_ath12k#bsd/dev/hakios_dada_wifi.c\1standard#' "$F"
  sed -i 's#bsd/dev/hakios_dada_btsk.c\([[:space:]]*\)optional hakios_ath12k#bsd/dev/hakios_dada_btsk.c\1standard#' "$F"
  sed -i 's#bsd/dev/hakios_dada_wlan.c\([[:space:]]*\)optional hakios_ath12k#bsd/dev/hakios_dada_wlan.c\1standard#' "$F"
done
echo "=== isolated conf now ==="
grep -n "hakios_dada_w" xnu-src/isolated/xnu-xnu-12377.121.6/bsd/conf/files
echo "=== rebuild ==="
bash docs/xuanyuan/tools/rebuild-verified-kernel.sh > /tmp/kwifi2.log 2>&1
echo "rc=$?"
grep -nE "error:|hakios_dada_wifi|undefined" /tmp/kwifi2.log | head -8
tail -3 /tmp/kwifi2.log
