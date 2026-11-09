#!/bin/bash
set -u
cd /home/hcy/hakios-arm64e-xuanyuan
H=userland/userland/src/rw_null_air.h
if [ ! -f "$H" ]; then
cat > "$H" <<'EOF'
/* placeholder for the generated fallback fragment shader AIR (src/rw_null.metal):
   an empty module, enough to build and load the plug-in while the real AIR is generated. */
static const unsigned char rw_null_air[] = { 0 };
static const unsigned long rw_null_air_len = 0;
EOF
fi
echo "header written: $(wc -c < $H) bytes"
sed -i 's/BUILD_METAL_PLUGIN=0/BUILD_METAL_PLUGIN=1/; s/METAL=0/METAL=1/' /mnt/x/ios/scripts/build-userland-gl.sh
bash /mnt/x/ios/scripts/build-userland-gl.sh > /tmp/metal-build2.log 2>&1
echo "build rc=$?"
grep -nE "error|fatal error|undefined" /tmp/metal-build2.log | head -12
tail -5 /tmp/metal-build2.log
ls -la userland/userland/root/lib 2>/dev/null | grep -iE "metal|Metal" | head
