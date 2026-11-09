#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/userland/userland/src/inject26.c
echo "=== springboard_fixes line numbers ==="
grep -n "springboard_fixes\|SBLockdownManager" "$F" | head -5
N=$(grep -n "^springboard_fixes(void)" "$F" | head -1 | cut -d: -f1)
echo "line: $N"
START=$((N-10))
END=$((N+70))
sed -n "${START},${END}p" "$F"
