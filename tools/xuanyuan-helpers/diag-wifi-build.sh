#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== root.apfs now ==="
ls -la userland/userland/root.apfs
echo "=== root dir size ==="
du -sh userland/userland/root 2>/dev/null
echo "=== fw files inside root staging ==="
ls userland/userland/root/fw 2>/dev/null | head -5
ls userland/userland/root/fw 2>/dev/null | wc -l
echo "=== build.sh size/limit checks ==="
grep -nE "size|SIZE|63|PAYLOAD_MAX|too (big|large)|exceed|blocks of" userland/userland/build.sh | head -12
echo "=== run RADIO=1 build, show errors ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /tmp/wifi-build.log 2>&1
echo "rc=$?"
grep -nE "error|Error|exceed|too big|bad size|FAILED|No space" /tmp/wifi-build.log | head -10
tail -6 /tmp/wifi-build.log
