#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
K=xnu-src/xnu-12377.121.6/bsd/dev
echo "=== gen80000 firmware already collected? ==="
find /mnt/x/air6 -maxdepth 5 -iname "gen80000*" -printf "%s %p\n" 2>/dev/null | head -10
find /home/hcy/hakios-arm64e-xuanyuan -iname "gen80000*" -printf "%s %p\n" 2>/dev/null | head -10
echo
echo "=== redwood_adreno.c: firmware / gmu / smmu hooks ==="
grep -nE "firmware|zap|gmu|GMU|sqe|aqe|load_fw|request_firmware|mdt" $K/redwood_adreno.c | head -30
echo
echo "=== redwood_adreno.c size / structure ==="
wc -l $K/redwood_adreno.c $K/hakios_dada_gpu.c xnu-src/xnu-12377.121.6/iokit/Kernel/RedwoodGPU.cpp
echo
echo "=== RedwoodGPU user client requests ==="
grep -nE "IOAccelerator|metal|Metal|userClient|kMsm|0x7[0-9a-f]" xnu-src/xnu-12377.121.6/iokit/Kernel/RedwoodGPU.cpp | head -20
