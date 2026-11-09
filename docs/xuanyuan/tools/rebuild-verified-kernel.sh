#!/usr/bin/env bash
set -uo pipefail
base=/home/hcy/hakios-arm64e-xuanyuan/base
export THEOS=/home/hcy/theos
export XNU_ARCH=arm64e
export XNU_SRC=/home/hcy/hakios-arm64e-xuanyuan/xnu-src/isolated/xnu-xnu-12377.121.6
export RC_DARWIN_KERNEL_VERSION=12377.121.6
out=/mnt/x/air6/xiaomi15ultra-hakios
"$base/xnu-build.sh" -j8 -k > "$out/rebuild-kernel.log" 2>&1
build_status=$?
python3 - "$out/rebuild-kernel.log" "$build_status" <<'PY'
from pathlib import Path
import sys,re
log=Path(sys.argv[1]).read_text(errors='replace')
errors=[line for line in log.splitlines() if re.search(r'error:|Undefined symbols|No rule to make|No such file',line)]
other=[line for line in errors if not (line.startswith('kextsymboltool: error: exported name not in import list:') or line == "kextsymboltool: error: couldn't write output: Success")]
if other:
 print('\n'.join(other[:30]));raise SystemExit(1)
print('Compiler/linker stage accepted; overall make exit='+sys.argv[2])
print('Outstanding KEXT export diagnostics: '+str(len(errors)))
PY
[ $? = 0 ] || exit 1
python3 "$out/fix-copyio-relocations.py" || exit 1
python3 "$base/xnuloader/mkpayload.py" -o "$out/phone-xnu-checkpoint/hello-reproducible.bin" \
 kernel="$out/kernel-build/mach.xuanyuan.copyio-fixed" \
 ramdisk="$out/phone-xnu-checkpoint/hello-arm64e" \
 --cmdline "cpus=1 -v loader_return_after=45" || exit 1
printf '%s\n' 'RAM test payload built. No phone command or partition write was executed.'
