#!/bin/bash
F=/mnt/x/ios/work/metal-refs/a8xx/adreno-a6xx.xml
echo "=== PWR_CLK_STATUS ==="
grep -n "PWR_CLK_STATUS" "$F" | head -10
echo
echo "=== CX_MISC entries ==="
grep -nE 'name="CX_MISC' "$F" | head -20
echo
echo "=== A8XX-style variants (positive) ==="
grep -nE 'variants="[A-Z0-9,]*A8XX[",]' "$F" | head -10
