#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
curl -sL --max-time 40 -A curl "https://api.github.com/repos/superboyyy/hakios-arm64e-dada/contents/gpu2?ref=main" -o /tmp/gpu2.json -w "%{http_code} %{size_download}\n"
python3 - <<'PY'
import json
try:
    d = json.load(open('/tmp/gpu2.json'))
    if isinstance(d, dict):
        print('ERR:', d.get('message'))
    else:
        for e in d:
            print(e['type'], e['name'])
except Exception as e:
    print('parse', e)
PY
