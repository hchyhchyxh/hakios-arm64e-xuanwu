#!/bin/bash
# Split the 5.1 GB iOS system image into <=3 GB pieces the loader takes one by one
set -e
SRC=/home/hcy/hakios-arm64e-xuanyuan/userland/userland/system.apfs
DST=/mnt/x/air6/xiaomi15ultra-hakios/resident
mkdir -p "$DST"
rm -f "$DST"/part.*
echo "=== splitting (pieces 3 GiB) ==="
split -b 3221225472 --numeric-suffixes=1 --suffix-length=2 "$SRC" "$DST/part."
ls -la "$DST"
echo "=== SHA256 of source ==="
sha256sum "$SRC"
echo "=== SPLIT DONE ==="
