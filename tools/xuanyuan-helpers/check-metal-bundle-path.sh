#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
U=$ROOT/userland
echo "=== build.sh references to RedwoodMetal.bundle / Extensions ==="
grep -n "RedwoodMetal.bundle\|System/Library/Extensions" "$U/build.sh" | head -10
echo "=== root System dir ==="
ls -d "$U/root/System/Library" 2>/dev/null && ls "$U/root/System/Library" | head
