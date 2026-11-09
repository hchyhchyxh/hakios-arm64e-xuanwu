#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_touch.c
echo "=== rpmh/rsc mentions ==="
grep -nE 'rpmh|RSC|TCS|tcs' "$F" | head -30
echo
echo "=== rpmh_write body (first 90 lines from match) ==="
N=$(grep -n 'rpmh_write(const' "$F" | head -1 | cut -d: -f1)
echo "fn at $N"
sed -n "$((N-30)),$((N+90))p" "$F"
