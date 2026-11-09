#!/bin/bash
# WSL toolchain + apt/network probe
echo "=== tools ==="
for t in git curl wget make clang clang++ lld ld.lld llvm-objcopy dtc bc python3 pip3 cmake ninja unzip xz zstd rsync qemu-system-aarch64 qemu-img bison flex autoconf automake libtool pkg-config; do
  if command -v "$t" >/dev/null 2>&1; then
    printf "%-22s OK  %s\n" "$t" "$(command -v "$t")"
  else
    printf "%-22s MISSING\n" "$t"
  fi
done
echo "=== versions ==="
clang --version 2>/dev/null | head -2
python3 --version 2>&1
qemu-system-aarch64 --version 2>/dev/null | head -1
echo "=== apt sources ==="
grep -rhE "^[^#]* (https?|deb)" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null | sort -u | head -20
echo "=== mirror connectivity ==="
for u in https://mirrors.tuna.tsinghua.edu.cn/ https://mirrors.aliyun.com/ https://repo.huaweicloud.com/ http://archive.ubuntu.com/ https://pypi.tuna.tsinghua.edu.cn/simple/; do
  printf "%-55s" "$u"
  curl -sS -m 8 -o /dev/null -w "http=%{http_code} %{time_total}s\n" "$u" 2>&1 | tail -1
done
echo "=== proxy env ==="
env | grep -i proxy || echo "no proxy env"
cat /etc/environment 2>/dev/null | head -5
echo "=== apt package cache ==="
dpkg -l 2>/dev/null | awk '$1=="ii"{print $2}' | grep -cE '^' || true
echo "=== free ==="
df -h /home | tail -1
echo "=== done ==="
