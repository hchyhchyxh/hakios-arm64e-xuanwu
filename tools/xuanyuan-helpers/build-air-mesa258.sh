#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
T=$(find /mnt/c/Users/Administrator/Desktop -maxdepth 1 -name "mesa-25.2.8.tar.xz" 2>/dev/null | head -1)
echo "tarball: $T"
[ -n "$T" ] || exit 1
W=$ROOT/xnu-deps/build/mesa258
rm -rf "$W"; mkdir -p "$W"
cp -f "$T" "$W/mesa-25.2.8.tar.xz"
cd "$W" || exit 1
tar -xJf mesa-25.2.8.tar.xz
M=mesa-25.2.8
echo "=== copy AIR frontend in ==="
rm -rf "$M/src/compiler/air"
cp -a "$ROOT/tools/mesa/air" "$M/src/compiler/air"
rm -rf "$M/src/redwood-drm"
cp -a "$ROOT/tools/mesa/redwood-drm" "$M/src/redwood-drm"
echo "=== apply dada patches (tolerant) ==="
for p in "$ROOT"/tools/mesa/*.patch; do
  patch -s -N -f --no-backup-if-mismatch -d "$M" -p1 < "$p" || echo "skip patch: $(basename "$p")"
done
echo "=== meson setup (AIR-only, no OSMesa needed) ==="
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
export PATH=$ROOT/xnu-deps/build/mesa/venv/bin:$PATH
meson setup b --cross-file /home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa/cross.ini \
  --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled \
  -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dbuild-tests=false \
  -Dlmsensors=disabled -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
  > meson-setup.log 2>&1
echo "meson rc=$?"
tail -6 meson-setup.log
echo "=== ninja libredwoodair ==="
ninja -C b src/gallium/drivers/freedreno/libredwoodair.dylib > ninja.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED" ninja.log | head -6
tail -4 ninja.log
ls -la b/src/gallium/drivers/freedreno/libredwoodair* 2>/dev/null
