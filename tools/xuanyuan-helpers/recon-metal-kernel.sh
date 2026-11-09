#!/bin/bash
cd /home/hcy/hakios-arm64e-xuanyuan
X=xnu-src/isolated/xnu-xnu-12377.121.6
echo "=== MetalPluginName / metal= / metal_procs references ==="
grep -rn "MetalPluginName\|MetalPluginClassName\|metal_procs\|metal_all" "$X"/iokit "$X"/bsd 2>/dev/null | head -20
echo
echo "=== metal_* boot args parsing ==="
grep -rnE 'PE_parse_boot_argn\("metal' "$X"/bsd/dev/*.c "$X"/iokit/Kernel/*.cpp 2>/dev/null | head -12
