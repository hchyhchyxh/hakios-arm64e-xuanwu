#!/bin/bash
# GPU probe with the sm8750 overrides on the rebuilt kernel
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
KERNEL=$BASE/kernel-build/mach.xuanyuan.copyio-fixed
ROOT=userland/userland/root.apfs
OUT=$BASE/phone-ios26-test/ios26-gpu-sm8750.bin
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 adreno=10 adreno_pause=0 adreno_watch=0 cmd_db=0x81c60000 gmu=0x3d6c000 gmu_size=0x32000 smmu_size=0x40000 pdc=0xb220000 qfprom=0x221c8000 tsens1=0xc229000 loader_return_after=600 epoch=$(date +%s)"
ls -la "$KERNEL" "$ROOT"
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$KERNEL" ramdisk="$ROOT" --cmdline "$ARGS"
ls -la "$OUT"
echo "=== GPU SM8750 PAYLOAD DONE ==="
