#!/bin/bash
R=/home/hcy/hakios-arm64e-xuanyuan/docs/xuanyuan/reference
echo "=== patch11: CM3_SYSRESET / gmu_write ==="
grep -n "CM3_SYSRESET\|CM3_BOOT_CONFIG\|gmu_write\|REG_A8XX" "$R/a8xx-patch11-gmu-support.txt" | head -12
echo
echo "=== patch10: 0x1f800 / offset hunks ==="
grep -nE "0x1f800|0x1f801|0x1f81|GMU_CM3|offset" "$R/a8xx-patch10-gmu-offsets.txt" | head -20
echo
echo "=== patch15: GBIF ==="
grep -nE "GBIF|gbif|0x[0-9a-f]{4,}" "$R/a8xx-patch15-cx-gbif.txt" | head -20
