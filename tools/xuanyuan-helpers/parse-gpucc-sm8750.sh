#!/bin/bash
D=/mnt/x/ios/work/metal-refs/sm8750
python3 - <<'PY'
import re
src = open('/mnt/x/ios/work/metal-refs/sm8750/gpucc-sm8750.c').read()
print("=== all clk definitions and their offsets ===")
for m in re.finditer(r'static struct (clk_[a-z0-9_]+|gdsc|clk_branch|clk_rcg2|clk_regmap) ([a-z0-9_]+) = \{(.*?)\n\};', src, re.S):
    body = m.group(3)
    fields = dict(re.findall(r'\.(halt_reg|enable_reg|cmd_rcgr|reg)\s*=\s*(0x[0-9a-f]+)', body))
    if fields:
        print(m.group(2), fields)
print()
print("=== cxo / pll / reset / gx mentions ===")
for line in src.splitlines():
    if any(k in line.lower() for k in ('cxo', 'pll', 'reset', 'gx_', '_gx', 'hw_ctrl', 'domain_addr')):
        print(line.strip()[:150])
PY
