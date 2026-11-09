#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
for T in xnu-src/xnu-12377.121.6 xnu-src/isolated/xnu-xnu-12377.121.6; do
  F="$T/bsd/dev/hakios_dada_gpu.c"
  echo "=== $F ==="
  if [ -f "$F" ]; then
    wc -l "$F"
    echo "gen8/qmp matches: $(grep -cE 'qmp|a8xx|gen80000|gmxc|QMP' "$F")"
    echo "--- boot args ---"
    grep -oE 'PE_parse_boot_argn\("[a-z_0-9]+"' "$F" | sort -u | head -12
    echo "--- gen8 lines ---"
    grep -nE 'a8xx|gen80000|gmxc|qmp_|QMP_|dadagpu' "$F" | head -12
  else
    echo "(absent)"
  fi
done
