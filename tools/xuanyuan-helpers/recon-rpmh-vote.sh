#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
echo "=== existing RPMh vote / RSC code in bsd/dev ==="
grep -rln "rpmh\|RPMh\|RSC_DRV\|tcs" xnu-src/xnu-12377.121.6/bsd/dev/*.c | head -10
echo
echo "=== RSC base / register addresses mentioned ==="
grep -rnE "0x[0-9a-f]*18200000|rsc@|RSC_DRV_CMD|TCS_CMD|rpmh_write|rpmh_vote|rpmh_send" xnu-src/xnu-12377.121.6/bsd/dev/*.c | head -20
echo
echo "=== hakios_dada_cpu.c rpmh context ==="
grep -n "rpmh\|RPMh" xnu-src/xnu-12377.121.6/bsd/dev/hakios_dada_cpu.c | head -15
echo
echo "=== redwood_wifi.c QMP code (for clock votes) ==="
grep -n "qmp\|QMP" xnu-src/xnu-12377.121.6/bsd/dev/redwood_wifi.c | head -15
