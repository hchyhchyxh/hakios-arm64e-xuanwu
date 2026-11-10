#!/bin/bash
# Metal, but the present path paced on the display's refresh with 4 copy threads (no dadascan):
# the 1125x2436 display is scaled onto the 1440x3200 panel by the CPU, so the copy is what has
# to fit inside one refresh; the beat-paced path (fScan empty) is the one that waits for it.
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
U=/home/hcy/hakios-arm64e-xuanyuan/userland/userland
OUT=${1:-$BASE/phone-ios26-test/ios26-metal-paced.bin}
ARGS="cpus=6 fb_threads=4 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 dadatouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 clocka11y=1 dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd metal_arch=0x01000017,0xc camtl=1 loader_return_after=1800 epoch=$(date +%s)"
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$BASE/kernel-build/mach.xuanyuan.copyio-fixed" ramdisk="$U/root.apfs" --cmdline "$ARGS"
ls -la "$OUT"
