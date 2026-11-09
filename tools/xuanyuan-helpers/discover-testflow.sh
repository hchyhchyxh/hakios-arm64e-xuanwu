#!/bin/bash
echo "=== /home/hcy top files modified since Nov 8 (sh/bin/py/img) ==="
find /home/hcy -maxdepth 2 -newermt "2026-11-08 00:00" -type f \( -name "*.sh" -o -name "*.bin" -o -name "*.img" -o -name "*.py" \) -printf "%TY-%Tm-%Td %TH:%TM %10s %p\n" 2>/dev/null | sort | tail -30
echo
echo "=== xuanyuan loader image anywhere in tree ==="
find /home/hcy/hakios-arm64e-xuanyuan -maxdepth 4 -name "*xuanyuan*.img" -o -maxdepth 4 -name "*usb*.img" -printf "%TY-%Tm-%Td %TH:%TM %10s %p\n" 2>/dev/null | sort | tail -10
echo
echo "=== X: air6 recent payload bins ==="
find /mnt/x/air6 -maxdepth 3 -newermt "2026-11-08 00:00" -type f \( -name "*.bin" -o -name "*.img" \) -printf "%TY-%Tm-%Td %TH:%TM %10s %p\n" 2>/dev/null | sort | tail -20
