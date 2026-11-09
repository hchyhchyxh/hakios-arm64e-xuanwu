#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/userland/userland/src/inject26.c
N=$(grep -n "^caenv_from_bootargs(void)" "$F" | head -1 | cut -d: -f1)
echo "line: $N"
sed -n "$((N-15)),$((N+45))p" "$F"
