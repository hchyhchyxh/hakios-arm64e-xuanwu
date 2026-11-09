#!/bin/bash
export http_proxy=http://172.25.144.1:12001
export https_proxy=$http_proxy
export HTTP_PROXY=$http_proxy HTTPS_PROXY=$https_proxy
cd /home/hcy/hakios-arm64e-xuanyuan || exit 1
echo "=== direct run, trace head ==="
bash -x userland/tools/mk-mesa.sh 2>&1 | head -60
echo "=== rc chain done ==="
echo "=== xnu-env ==="
cat userland/xnu-env.sh
echo "=== base/xnu-env.sh ==="
cat base/xnu-env.sh 2>/dev/null | head -25
