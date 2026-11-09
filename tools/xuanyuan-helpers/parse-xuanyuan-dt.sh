#!/bin/bash
# Extract and decompile the xuanyuan device trees from public prebuilt images
set -e
OUT=/home/hcy/xuanyuan-dt
SRC=/mnt/x/ios/port/kernel-xuanyuan-prebuilt
mkdir -p "$OUT" "$OUT/dtbo"
echo "=== base dtb.img -> dts ==="
dtc -I dtb -O dts "$SRC/dtb.img" -o "$OUT/base.dts" 2>"$OUT/dtc-base.err" || { echo "dtc base failed:"; head -5 "$OUT/dtc-base.err"; }
[ -f "$OUT/base.dts" ] && echo "base.dts: $(wc -l < "$OUT/base.dts") lines"

echo "=== dtbo.img entries ==="
python3 - "$SRC/dtbo.img" "$OUT" <<'PY'
import struct, sys, os
p, out = sys.argv[1], sys.argv[2]
d = open(p, 'rb').read()
magic, total, hdr, cnt, ent = struct.unpack('<5I', d[:20])
print("magic=%08x total=%d hdr=%d count=%d entry_size=%d" % (magic, total, hdr, cnt, ent))
for i in range(cnt):
    off = hdr + i * ent
    size, doff = struct.unpack('<2I', d[off:off+8])
    blob = d[doff:doff+size]
    fn = "%s/dtbo/%02d.dtb" % (out, i)
    open(fn, 'wb').write(blob)
    print("  entry %02d size=%d off=0x%x magic=%s" % (i, size, doff, blob[:4].hex()))
PY

echo "=== decompile overlays ==="
ok=0
for f in "$OUT"/dtbo/*.dtb; do
  dtc -I dtb -O dts "$f" -o "${f%.dtb}.dts" 2>/dev/null && ok=$((ok+1))
done
echo "decompiled $ok overlays"

echo "=== overlay models (unique) ==="
grep -h -m1 'model = ' "$OUT"/dtbo/*.dts 2>/dev/null | sed 's/^[[:space:]]*//' | sort -u | head -40

echo "=== base.dts key nodes ==="
grep -nE 'model = |compatible = .qcom,s(un|unp)|interrupt-controller@16000000|ufshc@1d84000|kgsl-3d0|smem_region|splash|cont_splash|mdss@|dsi0|panel@|synaptics|qcom,board-id|qcom,msm-id' "$OUT/base.dts" | head -40
echo "=== done ==="
