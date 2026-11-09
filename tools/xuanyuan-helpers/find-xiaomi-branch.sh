#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
BASE=https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource
for b in xuanyuan-v-oss xuanyuan-u-oss xuanyuan-s-oss xuanyuan-oss; do
  code=$(curl -sL --max-time 30 -A curl -o /tmp/readme-$b.md -w "%{http_code}" "$BASE/$b/README.md")
  echo "README $b -> $code $(stat -c%s /tmp/readme-$b.md 2>/dev/null || echo 0)"
done
echo
echo "=== branch list via API ==="
curl -sL --max-time 40 -A curl "https://api.github.com/repos/MiCode/Xiaomi_Kernel_OpenSource/branches?per_page=100" -o /tmp/branches.json -w "%{http_code} %{size_download}\n" || true
python3 - <<'PY'
import json
try:
    d = json.load(open('/tmp/branches.json'))
    names = [b.get('name','') for b in d] if isinstance(d, list) else []
    print('branches:', len(names))
    print('\n'.join([n for n in names if 'xuanyuan' in n.lower() or '8750' in n]))
except Exception as e:
    print('err', e)
PY
