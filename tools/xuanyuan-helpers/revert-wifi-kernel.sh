#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan
for T in xnu-src/xnu-12377.121.6 xnu-src/isolated/xnu-xnu-12377.121.6; do
  F="$T/bsd/conf/files"
  sed -i 's#bsd/dev/hakios_dada_wifi.c\([[:space:]]*\)standard#bsd/dev/hakios_dada_wifi.c\1optional hakios_ath12k#' "$F"
  sed -i 's#bsd/dev/hakios_dada_btsk.c\([[:space:]]*\)standard#bsd/dev/hakios_dada_btsk.c\1optional hakios_ath12k#' "$F"
  sed -i 's#bsd/dev/hakios_dada_wlan.c\([[:space:]]*\)standard#bsd/dev/hakios_dada_wlan.c\1optional hakios_ath12k#' "$F"
done
echo "=== restored conf ==="
grep -n "hakios_dada_w" xnu-src/isolated/xnu-xnu-12377.121.6/bsd/conf/files
