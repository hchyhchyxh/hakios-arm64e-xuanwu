#!/bin/bash
# Boot the HakiOS open kernel in QEMU using only prebuilt/ artifacts (no Apple data, no phone).
# usage: boot-open.sh SECONDS
set -e
SECS=${1:-90}
cd /home/hcy/hakios
mkdir -p logs qemu/esp
echo "=== payload: prebuilt kernel + prebuilt hello, open defconfig args ==="
ARGS_OPEN=$(python3 tools/hakios-config.py args --defconfig defconfig)
echo "ARGS_OPEN=$ARGS_OPEN"
python3 xnuloader/mkpayload.py -o qemu/payload.bin \
    kernel=prebuilt/hakios-kernel \
    ramdisk=prebuilt/userland/hello \
    --cmdline "$ARGS_OPEN"
ls -la qemu/payload.bin
echo "=== boot: qemu virt aarch64, ${SECS}s ==="
cd qemu
./run.sh "$SECS" 2>&1 | tee /mnt/x/ios/logs/qemu-open.log
echo "=== run.sh exit=$? ==="
