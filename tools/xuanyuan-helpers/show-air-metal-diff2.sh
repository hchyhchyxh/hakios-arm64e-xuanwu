#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
echo "=== nir_air.h struct air_constant_value full ==="
sed -n '418,480p' tools/mesa/air/nir_air.h
echo "=== air_metal.c uses of constants ==="
grep -n "\.undefined\|->undefined\|\.more\|->more\|->bits\|\.bits" tools/mesa/air/air_metal.c | head -20
echo "=== air_metal.c 555-600 ==="
sed -n '555,600p' tools/mesa/air/air_metal.c
