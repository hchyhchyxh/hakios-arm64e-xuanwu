#!/bin/bash
set -u
ROOT=/home/hcy/hakios-arm64e-xuanyuan/userland
W=$ROOT/xnu-deps/build/mesa
M=$W/mesa-26.2.5
[ -d "$M" ] || { echo "no mesa 26.2.5 tree"; exit 1; }
CC=$ROOT/xnu-deps/host/bin/xnu-clang
CCP=$ROOT/xnu-deps/host/bin/xnu-clang++
SDK=/home/hcy/theos/sdks/iPhoneOS16.5.sdk
cat > "$W/cross-arm64e.ini" <<EOF
[binaries]
c = ['$CC', '-arch', 'arm64e', '-isysroot', '$SDK']
cpp = ['$CCP', '-arch', 'arm64e', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64e', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64e'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF
cd "$W" || exit 1
rm -rf build26
export PATH=$W/venv/bin:$PATH
meson setup build26 "$M" --cross-file cross-arm64e.ini --wrap-mode=nofallback \
  --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
  -Dlmsensors=disabled -Dbuild-tests=false '-Dcpp_args=-Dpthread_condattr_setclock(a,b)=0' -Db_lundef=false \
  > meson26.log 2>&1
echo "meson rc=$?"
tail -6 meson26.log
ninja -C build26 src/gallium/drivers/freedreno/libannibaleair.dylib > ninja26.log 2>&1
echo "ninja rc=$?"
grep -E "error:|FAILED" ninja26.log | head -8
tail -5 ninja26.log
ls -la build26/src/gallium/drivers/freedreno/libannibaleair.dylib 2>/dev/null
