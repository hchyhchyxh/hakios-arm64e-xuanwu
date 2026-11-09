#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
X=xnu-src/isolated/xnu-xnu-12377.121.6
echo "=== conf files entries for driver sources ==="
grep -rn "hakios_dada_wifi\|redwood_wifi\|hakios_mhi" $X/bsd/conf/files $X/bsd/conf/* 2>/dev/null | head -10
echo
echo "=== how other dada drivers are listed (sample) ==="
grep -rn "hakios_dada_touch\|hakios_dada_gpu" $X/bsd/conf/files $X/bsd/conf/* 2>/dev/null | head -6
echo
echo "=== objects built in prior kernel build log ==="
grep -oE "hakios_[a-z_]+\.o|redwood_[a-z_]+\.o" /mnt/x/air6/xiaomi15ultra-hakios/rebuild-kernel.log 2>/dev/null | sort -u | head -30
echo
echo "=== wifi-related sources present ==="
ls $X/bsd/dev/ | grep -iE "mhi|pcie|wifi" | head -10
