#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
bash -x tools/mk-mesa.sh > /tmp/mkair-x.log 2>&1
echo "rc=$?"
echo "=== last lines ==="
tail -25 /tmp/mkair-x.log
