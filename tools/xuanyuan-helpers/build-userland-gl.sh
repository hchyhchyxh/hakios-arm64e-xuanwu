#!/bin/bash
# Build the iOS 26 userland root with the Mesa OpenGL ES renderer (GL=1): the software GL path
# that lets CoreAnimation composite the lock screen's glass/SDF clock when no GPU driver runs.
set -o pipefail
cd /home/hcy/hakios-arm64e-xuanyuan/userland/userland || exit 1
export THEOS=/home/hcy/theos
export HOME=/home/hcy
export XNU_ARCH=arm64e ARCH=arm64e
export IOS=/home/hcy/hakios-arm64e-xuanyuan/ios26
export CACHE=$IOS/cryptex/System/Library/Caches/com.apple.dyld
export LDID=/home/hcy/xuanyuan-host-tools/ldid
export BUILD_METAL_PLUGIN=1 RADIO=0 METAL=1 REST=1 GL=1
export USERLAND_EXTRA_CFLAGS="-I/home/hcy/xuanyuan-userland-sdk-compat/MacOSX26.1.sdk/usr/include -include /mnt/x/air6/xiaomi15ultra-hakios/userland-sdk-compat.h"
LOG=/mnt/x/ios/logs/userland-gl-build.log
mkdir -p /mnt/x/ios/logs
echo "=== build.sh GL=1 ===" | tee -a "$LOG"
bash build.sh 2>&1 | tee -a "$LOG" | tail -60
rc=${PIPESTATUS[0]}
echo "=== build.sh rc=$rc ===" | tee -a "$LOG"
echo "=== root.apfs ===" | tee -a "$LOG"
ls -la root.apfs 2>/dev/null | tee -a "$LOG"
echo "=== gl libs in root staging ===" | tee -a "$LOG"
find . -maxdepth 3 -name "libredwood-gl*" -o -maxdepth 3 -name "libOSMesa*" 2>/dev/null | head -5 | tee -a "$LOG"
echo "=== USERLAND GL BUILD DONE (rc=$rc) ===" | tee -a "$LOG"
