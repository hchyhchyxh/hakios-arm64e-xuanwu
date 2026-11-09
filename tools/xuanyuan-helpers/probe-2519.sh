#!/bin/bash
set -u
T=$(find /mnt/c/Users/Administrator/Desktop -maxdepth 1 -name "mesa-25.1.9.tar.xz" 2>/dev/null | head -1)
echo "tarball: $T"
[ -n "$T" ] || exit 1
ls -la "$T"
sha256sum "$T"
rm -rf /tmp/mesa-2519 && mkdir -p /tmp/mesa-2519
tar -xJf "$T" -C /tmp/mesa-2519
M=/tmp/mesa-2519/mesa-25.1.9
echo "=== osmesa target ==="
ls "$M/src/gallium/targets/osmesa" 2>/dev/null | head -3
echo "=== fp_math_ctrl ==="
grep -c "fp_math_ctrl" "$M/src/compiler/nir/nir_builder.h" 2>/dev/null || echo 0
echo "=== mesa_shader_stage ==="
grep -c "mesa_shader_stage" "$M/src/compiler/shader_enums.h" 2>/dev/null || echo 0
echo "=== air_constant_value.undefined ==="
grep -c "undefined" "$M/src/compiler/nir/nir.h" 2>/dev/null || true
