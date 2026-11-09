#!/bin/bash
F=/mnt/x/ios/work/metal-refs/a8xx/a6xx_gmu.xml
echo "=== GMU registers by name (offset in words) ==="
grep -oE '<reg32 offset="0x[0-9a-f]+" name="(GMU_CM3_SYSRESET|GMU_CM3_BOOT_CONFIG|GMU_CM3_FW_INIT_RESULT|GMU_CM3_CFG|GMU_ALWAYS_ON_COUNTER[A-Z_]*|GMU_HFI_[A-Z_]*|GMU_AO_[A-Z_]*)"[^>]*>' "$F" | head -20
echo
echo "=== all CM3 / ALWAYS_ON entries ==="
grep -nE 'CM3|ALWAYS_ON' "$F" | head -20
