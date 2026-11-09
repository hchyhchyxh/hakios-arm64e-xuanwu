#!/bin/bash
set -e
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== plat_fw_get / firmware names in hakios_dada_wifi.c ==="
grep -nE '"([a-z0-9_]+\.(bin|elf|msc|e[0-9a-f]+))"|fw_get|plat_fw_get|amss|bdwlan|ucode|regdb' xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_wifi.c | head -25
mkdir -p userland/fw-wifi-dada/image-peach
cp /mnt/x/air6/xiaomi15ultra-hakios/fw-wifi-peach/* userland/fw-wifi-dada/image-peach/
ls userland/fw-wifi-dada/image-peach | wc -l
du -sh userland/fw-wifi-dada/image-peach
echo
echo "=== boot args the driver reads ==="
grep -nE 'PE_parse_boot_argn\("dadawifi|dadawifi' xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_wifi.c | head -10
