#!/usr/bin/env bash
set -euo pipefail
cd /home/hcy/hakios-arm64e-xuanyuan/userland/userland
export THEOS=/home/hcy/theos
export XNU_ARCH=arm64e ARCH=arm64e
export IOS=/home/hcy/hakios-arm64e-xuanyuan/ios26
export CACHE=$IOS/cryptex/System/Library/Caches/com.apple.dyld
export LDID=/home/hcy/xuanyuan-host-tools/ldid
export BUILD_METAL_PLUGIN=0 RADIO=0 METAL=0 REST=1
export USERLAND_EXTRA_CFLAGS='-I/home/hcy/xuanyuan-userland-sdk-compat/MacOSX26.1.sdk/usr/include -include /mnt/x/air6/xiaomi15ultra-hakios/userland-sdk-compat.h'
exec bash build.sh
