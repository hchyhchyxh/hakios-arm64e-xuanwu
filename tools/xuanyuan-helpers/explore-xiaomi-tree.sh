#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
API=https://api.github.com/repos/MiCode/Xiaomi_Kernel_OpenSource/contents
REF=xuanyuan-v-oss
list() {
  local path="$1"
  curl -sL --max-time 40 -A curl "$API/$path?ref=$REF" -o /tmp/gh.json -w "%{http_code}" >/dev/null
  echo "--- /$path ---"
  python3 - <<'PY'
import json
try:
    d = json.load(open('/tmp/gh.json'))
    if isinstance(d, dict) and 'message' in d:
        print('ERR:', d['message']); raise SystemExit
    names = [e['name'] + ('/' if e['type']=='dir' else '') for e in d]
    print(' '.join(names[:60]))
except Exception as e:
    print('parse err', e)
PY
}
list ""
list "drivers"
list "drivers/gpu"
