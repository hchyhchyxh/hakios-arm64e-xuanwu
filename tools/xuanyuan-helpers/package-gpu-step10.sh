#!/bin/bash
# GPU step test: firmware in the image, run the a660-line driver's step 10 on the a8xx hardware
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
KERNEL=$BASE/kernel-build/mach.xuanyuan.copyio-fixed
ROOT=userland/userland/root.apfs
OUT=$BASE/phone-ios26-test/ios26-gpu-step10.bin
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 gpu=1 adreno_only=10 loader_return_after=900 epoch=$(date +%s)"
echo "=== firmware in root image? ==="
ls -la userland/userland/root/fw/ 2>/dev/null
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$KERNEL" ramdisk="$ROOT" --cmdline "$ARGS"
ls -la "$OUT"
echo "=== GPU STEP10 PAYLOAD DONE ==="
