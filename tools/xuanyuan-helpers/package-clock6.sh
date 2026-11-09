#!/bin/bash
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
R=/home/hcy/hakios-arm64e-xuanyuan/userland/userland/root.apfs
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 clockplain=1 clocka11y=1 sb_wallpaper=0 displaykeep=1 loader_return_after=600 epoch=$(date +%s)"
python3 base/xnuloader/mkpayload.py -o "$BASE/phone-ios26-test/ios26-clock6.bin" \
  kernel="$BASE/kernel-build/mach.xuanyuan.copyio-fixed" ramdisk="$R" --cmdline "$ARGS"
ls -la "$BASE/phone-ios26-test/ios26-clock6.bin"
