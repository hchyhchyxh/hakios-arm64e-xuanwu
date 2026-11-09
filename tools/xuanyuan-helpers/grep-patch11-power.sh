#!/bin/bash
R=/home/hcy/hakios-arm64e-xuanyuan/docs/xuanyuan/reference
F="$R/a8xx-patch11-gmu-support.txt"
echo "=== function/hunk headers ==="
grep -nE '^@@|^\+static |^[-+]?[a-z_]+\(.*\)$|power|POWER|GBIF|reset|HFI' "$F" | head -40
echo
echo "=== lines with GMU register writes for a8xx ==="
grep -nE 'REG_A8XX|gmu_write|gmu_read|RPMH|gdsc|GDSC|regulator|clk' "$F" | head -40
