#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
R=$ROOT/userland/root
echo "=== /System/Library/Extensions ==="
ls -la "$R/System/Library/Extensions" 2>/dev/null | head -8
ls "$R/System/Library/Extensions" 2>/dev/null | grep -iE "redwood|metal" | head
echo "=== root/lib metal files ==="
ls -la "$R/lib" | grep -iE "metal|air"
echo "=== inject.conf ==="
cat "$R/etc/inject.conf" 2>/dev/null | head -3
echo "=== package Metal payload (no gl=1) ==="
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd clockplain=1 clocka11y=1 sb_wallpaper=0 loader_return_after=600 epoch=$(date +%s)"
python3 base/xnuloader/mkpayload.py -o "$BASE/phone-ios26-test/ios26-metal-air.bin" \
  kernel="$BASE/kernel-build/mach.xuanyuan.copyio-fixed" ramdisk="$ROOT/userland/root.apfs" --cmdline "$ARGS"
ls -la "$BASE/phone-ios26-test/ios26-metal-air.bin"
