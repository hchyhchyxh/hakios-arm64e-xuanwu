#!/bin/bash
# Package the GL=1 clock test payload: verified kernel + GL-enabled root + gl=1 boot args
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
KERNEL=$BASE/kernel-build/mach.xuanyuan.copyio-fixed
ROOT=userland/userland/root.apfs
OUT=$BASE/phone-ios26-test/ios26-clock-gl.bin
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 loader_return_after=900 epoch=$(date +%s)"
echo "=== inputs ==="
ls -la "$KERNEL" "$ROOT"
echo "=== root contains GL libs? ==="
ls -la userland/userland/root/lib/libOSMesa.8.dylib userland/userland/root/lib/libredwood-gl.dylib 2>&1
echo "=== mkpayload ==="
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$KERNEL" ramdisk="$ROOT" --cmdline "$ARGS"
echo "=== payload ==="
ls -la "$OUT"
echo "=== GL PAYLOAD DONE ==="
