#!/bin/bash
set -u
U=/home/hcy/hakios-arm64e-xuanyuan/userland/userland
echo "=== build.sh lib handling ==="
grep -n "libredwood\|root/lib\|libOSMesa" "$U/build.sh" | head -20
echo "=== does build.sh know AIR? ==="
grep -n "libredwoodair\|air" "$U/build.sh" | head -10
