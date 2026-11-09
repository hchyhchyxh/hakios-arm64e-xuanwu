#!/bin/bash
# Run the GPU probe payload and print only the driver's decisive lines.
# usage: run-gpu-probe.sh <payload.bin> <logdir> [base-args...]
set -uo pipefail
FB=/mnt/x/air6/ui-functional-test-20261107/adb35/platform-tools/fastboot.exe
PAYLOAD="$1"; DIR="$2"; shift 2
mkdir -p "$(dirname "$DIR")" 2>/dev/null || true
"$FB" oem stop >/dev/null 2>&1
sleep 3
"$FB" stage "$PAYLOAD" 2>&1 | tail -1
"$FB" oem run 2>&1 | tail -1
echo "run start: $(date +%H:%M:%S)"
sleep 38
python3 /mnt/x/air6/xiaomi15ultra-hakios/phone-ios26-test/poll-loader.py --fastboot "$FB" --out "$DIR" --seconds 55 --interval 3 >/dev/null 2>&1 || true
echo "=== adreno lines ==="
grep -ah "adreno:" "$DIR"/*.log 2>/dev/null | sed 's/^.*adreno:/adreno:/' | awk '!seen[$0]++' | tail -70
echo "=== panic? ==="
grep -ah "PANIC" "$DIR"/*.log 2>/dev/null | awk '!seen[$0]++' | head -3
