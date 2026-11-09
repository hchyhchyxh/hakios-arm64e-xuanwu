#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== build.sh: fw handling ==="
grep -n "fw-" userland/userland/build.sh | head -10
grep -n "fw/" userland/userland/build.sh | head -10
echo
echo "=== root layout: where fw goes ==="
grep -n "redwood/fw\|/fw" userland/userland/build.sh | head -
echo
echo "=== driver: stage/step functions and chip test ==="
K=xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
grep -nE "^static .*\(|chip|0x06030500|0x44050001|a8xx|gen80000|stage|step" $K | head -60
