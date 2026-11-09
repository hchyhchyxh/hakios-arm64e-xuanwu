#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
echo "=== HakiOS nir_air.h: air_constant_value ==="
grep -n -A10 "struct air_constant_value" tools/mesa/air/nir_air.h | head -14
echo
echo "=== air_metal.c: 90-100 ==="
sed -n '90,100p' tools/mesa/air/air_metal.c
echo "=== 440-480 ==="
sed -n '440,480p' tools/mesa/air/air_metal.c
echo "=== 535-555 ==="
sed -n '535,555p' tools/mesa/air/air_metal.c
echo "=== includes top ==="
sed -n '45,60p' tools/mesa/air/air_metal.c
