#!/bin/bash
D=/home/hcy/hakios-arm64e-xuanyuan/userland/xnu-deps/build/mesa
echo "=== is Mesa patched (nir_builder fp_math_ctrl)? ==="
grep -c "fp_math_ctrl" "$D"/mesa-25.0.7/src/compiler/nir/nir_builder.h 2>/dev/null
echo "=== mesa_shader_stage_name in shader_enums.h? ==="
grep -c "mesa_shader_stage_name" "$D"/mesa-25.0.7/src/compiler/shader_enums.h 2>/dev/null
echo "=== .redwood-patches ==="
cat "$D"/mesa-25.0.7/.redwood-patches 2>/dev/null
echo "=== patch logs present? ==="
ls -la "$D"/*.log "$D"/patch* 2>/dev/null | head
echo "=== our upstream patch file refers to which version? ==="
head -8 /home/hcy/hakios-arm64e-xuanyuan/userland/tools/mesa/air-frontend.patch 2>/dev/null
head -6 /home/hcy/hakios-arm64e-xuanyuan/userland/tools/mesa/ir3-round-default.patch 2>/dev/null
