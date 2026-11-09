#!/bin/bash
K=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_adreno.c
echo "=== entry / arg parsing (tail of file) ==="
sed -n '4020,4206p' "$K"
