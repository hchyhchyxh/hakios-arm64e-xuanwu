#!/bin/bash
# skip step 2 (fuses) and run the rest of 1..10 with the sm8750 overrides
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
BASE=/mnt/x/air6/xiaomi15ultra-hakios
KERNEL=$BASE/kernel-build/mach.xuanyuan.copyio-fixed
ROOT=userland/userland/root.apfs
OUT=$BASE/phone-ios26-test/ios26-gpu-sm8750c.bin
ARGS="cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 adreno=10 adreno_pause=0 adreno_watch=0 adreno_skip=0x4 cmd_db=2177236992 gmu=64405504 gmu_size=204800 smmu_size=262144 pdc=186777600 qfprom=572293120 tsens1=203591680 loader_return_after=600 epoch=$(date +%s)"
python3 base/xnuloader/mkpayload.py -o "$OUT" kernel="$KERNEL" ramdisk="$ROOT" --cmdline "$ARGS"
ls -la "$OUT"
echo "=== SKIP2 PAYLOAD DONE ==="
