#!/bin/bash
# Fetch Metal-related reference docs from GitHub through the WSL proxy
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
echo "proxy: $http_proxy"
OUT=/mnt/x/ios/work/metal-refs
mkdir -p "$OUT"
fetch() {
	local url="$1" name="$2"
	curl -sL --max-time 40 -A "curl" "$url" -o "$OUT/$name" -w "%{http_code} %{size_download} $name\n" || true
}
fetch "https://raw.githubusercontent.com/steelbrain/metal2vulkan/main/README.md" metal2vulkan-README.md
fetch "https://raw.githubusercontent.com/steelbrain/metal2vulkan/master/README.md" metal2vulkan-README-master.md
fetch "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/docs/reference/metal-measurements.md" hakios-metal-measurements.md
fetch "https://raw.githubusercontent.com/oosexgang/hakios-mt6768/main/README.md" mt6768-README.md
fetch "https://raw.githubusercontent.com/oosexgang/hakios-mt6768/master/README.md" mt6768-README-master.md
fetch "https://raw.githubusercontent.com/iola1999/hackios-redmi-k90-annibale/main/README.md" k90-README.md
echo "=== files ==="
ls -la "$OUT"
