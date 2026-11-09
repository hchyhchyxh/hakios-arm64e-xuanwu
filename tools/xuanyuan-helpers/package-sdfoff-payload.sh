#!/bin/bash
# Package the SDF-off test payload (no rebuild: same root, extra CA environment switches)
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
KERNEL=$BASE/kernel-build/mach.xuanyuan.copyio-fixed
ROOT=userland/userland/root.apfs
OUT=$BASE/phone-ios26-test/ios26-clock-sdfoff.bin
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 caenv=CA_DISABLE_INLINE_SDFS=1 caenv=CA_DISABLE_SDF=1 caenv=CA_DISABLE_BACKDROP_CACHE=1 gl=1 clockplain=1 clocka11y=1 loader_return_after=900 epoch=$(date +%s)"
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$KERNEL" ramdisk="$ROOT" --cmdline "$ARGS"
ls -la "$OUT"
echo "=== SDFOFF PAYLOAD DONE ==="
