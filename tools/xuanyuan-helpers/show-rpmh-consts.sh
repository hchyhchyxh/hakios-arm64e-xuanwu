#!/bin/bash
F=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_touch.c
grep -nE 'MSGID_|tcs_reg =|struct tcs|CMD_DB|TAG_|CMD_DB' "$F" | head -20
echo "---"
N=$(grep -n 'MSGID_WRITE_4_BYTES' "$F" | head -1 | cut -d: -f1)
sed -n "$((N-10)),$((N+12))p" "$F"
