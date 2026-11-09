#!/bin/bash
# Scan a blob for FDTs, extract each, decompile; then grep the base DTS for the facts we need
OUT=/home/hcy/xuanyuan-dt
SRC=/mnt/x/ios/port/kernel-xuanyuan-prebuilt
mkdir -p "$OUT/dtbo2"

echo "=== base.dts: key nodes ==="
grep -nE 'model = |interrupt-controller@16000000|ufshc@1d84000|kgsl|smem_region|splash|cont_splash|mdss@|dsi|panel|synaptics|qcom,board-id|qcom,msm-id|remoteproc-adsp|qcom,ufshc' "$OUT/base.dts" | head -60

echo "=== dtbo2: FDT scan ==="
python3 - "$SRC/dtbo.img" "$OUT/dtbo2" <<'PY'
import struct, sys, os
p, out = sys.argv[1], sys.argv[2]
d = open(p, 'rb').read()
magic = b'\xd0\x0d\xfe\xed'
idx = 0
pos = 0
while True:
    i = d.find(magic, pos)
    if i < 0: break
    size = struct.unpack('>I', d[i+4:i+8])[0]
    if 64 <= size <= 20*1024*1024 and i + size <= len(d):
        blob = d[i:i+size]
        fn = os.path.join(out, '%02d.dtb' % idx)
        open(fn, 'wb').write(blob)
        print('fdt %02d at 0x%x size %d' % (idx, i, size))
        idx += 1
        pos = i + size
    else:
        pos = i + 4
print('total fdt:', idx)
PY

echo "=== decompile dtbo2 ==="
n=0
for f in "$OUT"/dtbo2/*.dtb; do
  [ -f "$f" ] || continue
  dtc -I dtb -O dts "$f" -o "${f%.dtb}.dts" 2>/dev/null && n=$((n+1))
done
echo "decompiled: $n"

echo "=== overlay models ==="
for f in "$OUT"/dtbo2/*.dts; do
  [ -f "$f" ] || continue
  m=$(grep -m1 'model = ' "$f" | sed 's/^[[:space:]]*//')
  bid=$(grep -m1 'qcom,board-id' "$f" | sed 's/^[[:space:]]*//')
  mib=$(grep -m1 'xiaomi,miboard-id' "$f" | sed 's/^[[:space:]]*//')
  echo "$(basename $f): $m | $bid | $mib"
done | head -50

echo "=== splash/panel/node in overlays matching xuanyuan/miboard 4 ==="
grep -l 'miboard-id = <0x4' "$OUT"/dtbo2/*.dts 2>/dev/null | head -5
grep -nE 'splash|panel|dsi|touch|synaptics|display' "$OUT"/dtbo2/*.dts 2>/dev/null | head -40
echo "=== done ==="
