#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== wifi docs ==="
ls docs/xuanyuan | grep -iE "wifi|wlan|wcn|wcn" || true
echo
echo "=== build.sh fw-wifi handling ==="
sed -n '711,745p' userland/userland/build.sh
echo
echo "=== wifi drivers in kernel ==="
ls xnu-src/xnu-12377.121.6/bsd/dev | grep -iE "wifi|wlan|wcn" || true
echo
echo "=== docs mentioning wifi ==="
grep -rln -i "wifi\|wlan\|wcn" docs/xuanyuan/*.md 2>/dev/null | head -5
echo
echo "=== device wifi chip hints ==="
grep -rn -i "wcn7850\|wcn7750\|wcn6855\|wcn6750" docs/xuanyuan userland/userland/build.sh 2>/dev/null | head -8
