#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan/userland || exit 1
# does the current air-frontend patch add the new APIs?
grep -l "fp_math_ctrl\|mesa_shader_stage_name" tools/mesa/*.patch 2>/dev/null
echo "---"
# force a clean Mesa tree so mk-mesa re-extracts and re-applies the current patch set
rm -rf xnu-deps/build/mesa/mesa-25.0.7 xnu-deps/build/mesa/build-softpipe-freedreno xnu-deps/build/mesa/out
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export THEOS=/home/hcy/theos HOME=/home/hcy ARCH=arm64e XNU_ARCH=arm64e AIR=1
LOG=/mnt/x/ios/logs/air-clean.log
bash tools/mk-mesa.sh > "$LOG" 2>&1
RC=$?
echo "mk-mesa rc=$RC log=$(stat -c%s "$LOG")"
tail -12 "$LOG"
echo "=== out ==="
ls -la xnu-deps/build/mesa/out 2>/dev/null | head -10
