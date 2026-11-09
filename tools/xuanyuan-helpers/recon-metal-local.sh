#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/metal-refs
mkdir -p "$OUT"
curl -sL --max-time 40 -A "curl" "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/docs/metal-plan.md" -o "$OUT/hakios-metal-plan.md" -w "%{http_code} %{size_download} metal-plan.md\n" || true
curl -sL --max-time 40 -A "curl" "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/docs/boot-args.md" -o "$OUT/hakios-boot-args.md" -w "%{http_code} %{size_download} boot-args.md\n" || true

cd /home/hcy/hakios-arm64e-xuanyuan
echo
echo "=== local tree: metal-related files ==="
ls userland/userland/src | grep -iE "metal|gpu|adreno|m2v" || true
echo "--- dirs ---"
ls -d hakios-metal gpu2 tools/m2v tools/mesa 2>/dev/null || true
echo "--- scripts ---"
ls userland/tools | grep -iE "turnip|m2v|metallib|ca-" || true
echo
echo "=== build.sh METAL_PLUGIN support ==="
grep -n "METAL_PLUGIN\|TURNIP\|M2V\|HakiosMetalCA" userland/userland/build.sh | head -20
echo
echo "=== boot arg metal= handling in userland/kernel ==="
grep -rn "\"metal=\|metal_m2v\|ca_metallib" userland/userland/src/*.c userland/userland/src/*.m xnu-src/xnu-12377.121.6/bsd/dev/*.c xnu-src/xnu-12377.121.6/iokit/Kernel/*.cpp 2>/dev/null | head -20
echo
echo "=== kernel GPU driver files ==="
ls xnu-src/xnu-12377.121.6/bsd/dev | grep -iE "gpu|adreno" || true
ls xnu-src/xnu-12377.121.6/iokit/Kernel | grep -iE "gpu|adreno|surface" || true
