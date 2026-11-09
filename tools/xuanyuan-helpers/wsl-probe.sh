#!/bin/bash
# WSL recon probe: quoting sanity + proxy reachability
echo "=== identity ==="
echo "HOME=$HOME"
x=abc
echo "x=[$x]"
echo "whoami=$(whoami)"
echo "pwd=$(pwd)"
echo "=== mounts ==="
ls -la /mnt/x/ios 2>&1 | head -5
echo "=== wsl interop ==="
cat /proc/sys/fs/binfmt_misc/WSLInterop 2>/dev/null | head -2
echo "=== proxy candidates ==="
for p in 127.0.0.1:12000 172.31.128.1:12000 10.255.255.254:12000; do
  echo "-- http://$p --"
  curl -sS -m 10 -x "http://$p" -o /dev/null -w "http=%{http_code} time=%{time_total}s\n" \
    https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/README.md 2>&1
done
echo "=== direct ==="
curl -sS -m 10 -o /dev/null -w "direct http=%{http_code}\n" https://github.com 2>&1
echo "=== tcp probes ==="
for hp in 172.31.128.1:12000 127.0.0.1:12000; do
  h="${hp%%:*}"; pt="${hp##*:}"
  timeout 3 bash -c "echo > /dev/tcp/$h/$pt" >/dev/null 2>&1 && echo "OPEN $hp" || echo "CLOSED $hp"
done
echo "=== done ==="
