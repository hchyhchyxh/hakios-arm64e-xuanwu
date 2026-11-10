#!/bin/bash
# Build libredwoodair.dylib (the Metal shader compiler) from the dada AIR frontend on Mesa
# 26.2.5, for arm64e iOS 26. See docs/xuanyuan/metal-air-mesa26-build.md for what each step
# is for and where it currently stops.
#
#   bash docs/xuanyuan/tools/build-air-mesa26.sh [/path/to/mesa-26.2.5.tar.xz] [/path/to/dada/userland]
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd)
U=$R/userland
TARBALL=${1:-/mnt/x/ios/work/mesa-26.2.5.tar.xz}
DADA=${2:-/mnt/x/ios/port/hakios-arm64e-dada/userland}
W=$U/xnu-deps/build/mesa-dada26
CC=$U/xnu-deps/host/bin/xnu-clang
CCP=$U/xnu-deps/host/bin/xnu-clang++
SDK=${THEOS:-/home/hcy/theos}/sdks/iPhoneOS16.5.sdk
K90AIR=${K90_AIR:-/mnt/x/ios/work/hackios-userland-k90/hackios-xnu-userland-k90-port/tools/mesa/air}

[ -f "$TARBALL" ] || { echo "Mesa 26.2.5 tarball not found: $TARBALL"; exit 1; }
[ -d "$DADA/tools/mesa/air" ] || { echo "dada userland not found: $DADA"; exit 1; }

rm -rf "$W"; mkdir -p "$W"; cd "$W" || exit 1
cp -f "$TARBALL" mesa-26.2.5.tar.xz
tar -xJf mesa-26.2.5.tar.xz
M=mesa-26.2.5

rm -rf "$M/src/compiler/air"; cp -a "$DADA/tools/mesa/air" "$M/src/compiler/air"
rm -rf "$M/src/redwood-drm";  cp -a "$DADA/tools/mesa/redwood-drm" "$M/src/redwood-drm"
for p in "$DADA"/tools/mesa/*.patch; do
  patch -s -N -f --no-backup-if-mismatch -d "$M" -p1 < "$p" || echo "skip $(basename "$p")"
done
# the dada air set ships no nir_air.h; the K90 one has the Mesa 26 types, extended below
cp "$K90AIR/nir_air.h" "$M/src/compiler/air/nir_air.h"

python3 - "$M" <<'PY'
import sys
M = sys.argv[1]
p = M + "/src/compiler/air/nir_air.h"
s = open(p).read()
s = s.replace("struct air_constant_value {\n   unsigned index;     /* the function constant's index */\n   uint64_t bits;      /* its value, as the bits of its type */\n};",
 "struct air_constant_value {\n   unsigned index;\n   uint64_t bits;\n   bool undefined;\n   uint32_t more[8];\n};")
s = s.replace("struct air_linked_function {\n   const char *name;\n   unsigned num_nodes;\n   const struct air_stitch_node *nodes;\n   unsigned output;\n};",
 "struct air_linked_function {\n   const char *name;\n   unsigned num_nodes;\n   const struct air_stitch_node *nodes;\n   unsigned output;\n   const void *bitcode;\n   size_t size;\n   const char *entry;\n   const struct air_constant_value *constants;\n   unsigned num_constants;\n};")
i = s.find("struct air_resource {")
if i >= 0 and "unused" not in s[i:i+800]:
    j = s.find("};", i); s = s[:j] + "   bool unused;\n" + s[j:]
open(p, "w").write(s)

# libdrm stand-in: Mesa 26 wants the syncobj API
h = M + "/src/redwood-drm/xf86drm.h"
s = open(h).read()
if "drmSyncobjTimelineWait" not in s:
    s += """
int drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, const uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery2(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);
"""
    open(h, "w").write(s)
c = M + "/src/redwood-drm/redwood_drm.c"
s = open(c).read()
if "drmSyncobjTimelineWait" not in s:
    s += """
int drmSyncobjWait(int fd, const uint32_t *h, unsigned n, int64_t t, unsigned f, uint32_t *first) { (void)fd;(void)h;(void)n;(void)t;(void)f; if (first) *first = 0; return 0; }
int drmSyncobjSignal(int fd, const uint32_t *h, uint32_t n) { (void)fd;(void)h;(void)n; return 0; }
int drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t n) { (void)fd;(void)h;(void)p;(void)n; return 0; }
int drmSyncobjTimelineWait(int fd, const uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *first) { (void)fd;(void)h;(void)p;(void)n;(void)t;(void)f; if (first) *first = 0; return 0; }
int drmSyncobjQuery2(int fd, const uint32_t *h, uint64_t *p, uint32_t n, uint32_t f) { (void)fd;(void)h;(void)p;(void)n;(void)f; return 0; }
int drmSyncobjTransfer(int fd, uint32_t dh, uint64_t dp, uint32_t sh, uint64_t sp, uint32_t f) { (void)fd;(void)dh;(void)dp;(void)sh;(void)sp;(void)f; return 0; }
"""
    open(c, "w").write(s)

u = M + "/src/util/u_sync_provider.c"
s = open(u).read()
if "#undef HAVE_LIBDRM" not in s:
    s = s.replace('#include "util/u_memory.h"', "#undef HAVE_LIBDRM\n#define HAVE_LIBDRM 1\n\n#include \"util/u_memory.h\"", 1)
    open(u, "w").write(s)

mb = M + "/src/gallium/drivers/freedreno/meson.build"
s = open(mb).read()
s = s.replace("inc_gallium, include_directories('../../../freedreno')",
              "inc_gallium, include_directories('../../../freedreno')", 1)  # no-op, keeps context
s = s.replace("include_directories('../../../freedreno/ir3'),",
              "include_directories('../../../freedreno/ir3'), include_directories('../../../freedreno/common'), include_directories('ir3'),", 1)
s = s.replace("link_with : [libgallium, libgalliumvl_stub, libws_null, libfreedrenowinsys],",
              "link_with : [libgallium, libgalliumvl_stub, libws_null, libfreedrenowinsys],\n  link_whole : [libfreedreno_ir3, _libnir, _libmesa_util],", 1)
s = s.replace("redwoodair_link_args += '-Wl,-install_name,/redwood/lib/libredwoodair.dylib'",
              "redwoodair_link_args += '-Wl,-install_name,/redwood/lib/libredwoodair.dylib'\n"
              "  redwoodair_link_args += ['-framework', 'IOKit', '-framework', 'Foundation', '-framework', 'CoreFoundation']", 1)
open(mb, "w").write(s)
print("source patches applied")
PY

cat > cross-arm64e.ini <<EOF
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

export PATH=$U/xnu-deps/build/mesa/venv/bin:$PATH
meson setup b mesa-26.2.5 --cross-file cross-arm64e.ini --wrap-mode=nofallback \
  --prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dllvm=disabled -Dshared-glapi=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
  -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
  -Dlmsensors=disabled -Dbuild-tests=false '-Dcpp_args=-Dpthread_condattr_setclock(a,b)=0' -Db_lundef=false \
  > meson.log 2>&1
echo "meson rc=$? (see meson.log)"
ninja -C b src/gallium/drivers/freedreno/libredwoodair.dylib > ninja.log 2>&1
echo "ninja rc=$? (see ninja.log)"
ls -la b/src/gallium/drivers/freedreno/libredwoodair.dylib 2>/dev/null
