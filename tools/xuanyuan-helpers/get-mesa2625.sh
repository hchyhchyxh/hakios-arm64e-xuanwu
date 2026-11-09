#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
cd /mnt/x/ios/work || exit 1
echo "proxy=$http_proxy"
if [ ! -f mesa-26.2.5.tar.xz ]; then
  curl -sL --fail -o mesa-26.2.5.tar.xz https://archive.mesa3d.org/mesa-26.2.5.tar.xz
  echo "curl rc=$?"
fi
ls -la mesa-26.2.5.tar.xz 2>/dev/null
sha256sum mesa-26.2.5.tar.xz 2>/dev/null
