#!/bin/bash
X=/mnt/x/ios/work/metal-refs/a8xx/adreno-a6xx.xml
echo "=== GBIF_QSB_SIDE registers ==="
grep -nE 'name="(GBIF_QSB_SIDE[0-9]|GBIF_QSB_SIDE|GBIF_)[A-Z0-9_]*"' "$X" | head -20
echo
echo "=== GBIF block base ==="
grep -n 'name="GBIF"' "$X" | head -3
