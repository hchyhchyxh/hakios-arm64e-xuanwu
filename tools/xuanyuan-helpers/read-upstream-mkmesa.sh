#!/bin/bash
F=/mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mk-mesa.sh
ls -la "$F" 2>/dev/null
echo "=== pins ==="
grep -nE 'VERSION=|SHA256=|DRIVERS=|AIR' "$F" 2>/dev/null | head -12
echo "=== air dir in upstream snapshot ==="
ls /mnt/x/ios/work/dada-repo/hakios-arm64e-dada-main/userland/tools/mesa/ 2>/dev/null | head
