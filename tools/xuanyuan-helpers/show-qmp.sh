#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/redwood_wifi.c
echo "=== QMP functions ==="
grep -nE "^qmp|^static.*qmp|qmp_[a-z_]+\(|QMP_[A-Z_]+|MSG|msg" "$F" | head -40
echo
echo "=== a send/vote function body ==="
N=$(grep -n "qmp_send" "$F" | head -1 | cut -d: -f1)
echo "send at $N"
sed -n "$((N-25)),$((N+60))p" "$F"
