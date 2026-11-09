#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
OUT=/mnt/x/ios/work/metal-refs
mkdir -p "$OUT"
fetch() {
	local url="$1" name="$2"
	curl -sL --max-time 40 -A "curl" "$url" -o "$OUT/$name" -w "%{http_code} %{size_download} $name\n" || true
}
fetch "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/docs/reference/metal.md" hakios-metal-howto.md
fetch "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/main/README.md" hakios-kernel-README.md
echo "=== repo listing via API ==="
curl -sL --max-time 40 -A "curl" "https://api.github.com/repos/oosexgang/hakios-xnu-kernel/git/trees/main?recursive=1" -o "$OUT/hakios-kernel-tree.json" -w "%{http_code} %{size_download} tree.json\n" || true
python3 - <<'PY'
import json
try:
    d = json.load(open('/mnt/x/ios/work/metal-refs/hakios-kernel-tree.json'))
    paths = [e['path'] for e in d.get('tree', [])]
    interesting = [p for p in paths if any(k in p.lower() for k in ('metal', 'gpu', 'adreno', 'air', 'turnip', 'vulkan'))]
    print('interesting paths:', len(interesting))
    print('\n'.join(interesting[:80]))
except Exception as e:
    print('parse error:', e)
PY
