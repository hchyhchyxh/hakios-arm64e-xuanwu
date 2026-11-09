#!/bin/bash
set -u
U=/home/hcy/hakios-arm64e-xuanyuan/userland/userland
echo "=== does build.sh install taps.txt? ==="
grep -n "taps.txt" "$U/build.sh" | head -5
echo "=== existing taps file? ==="
ls -la "$U/../taps.txt" "$U/taps.txt" 2>/dev/null
# the swipe that unlocks the lock screen, then a pause for the home screen
cat > "$U/../taps.txt" <<'EOF'
wait 18
swipe 50 88 50 30 260
wait 6
say swipe sent
EOF
sed -n '1,10p' "$U/../taps.txt"
echo "=== rebuild image (taps included if build.sh installs it) ==="
bash /mnt/x/ios/scripts/build-userland-gl.sh > /mnt/x/ios/logs/root-taps.log 2>&1
echo "build rc=$?"
tail -3 /mnt/x/ios/logs/root-taps.log
ls -la "$U/root/redwood/taps.txt" 2>/dev/null || ls -la "$U/root/taps.txt" 2>/dev/null || echo "(taps not in root)"
ls -la "$U/root.apfs"
