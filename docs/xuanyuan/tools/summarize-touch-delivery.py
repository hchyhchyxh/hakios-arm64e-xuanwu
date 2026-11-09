#!/usr/bin/env python3
"""Measure captured input delivery stages; does not claim input-to-photon latency."""
import argparse,json,re,statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('logs',nargs='+',type=Path);o=p.parse_args()
rows=set()
for path in o.logs:
 for match in re.finditer(r'tl: i (\d+) posted (\d+) read (\d+) sent (\d+)',path.read_text(errors='replace')):rows.add(tuple(map(int,match.groups())))
valid=[r for r in rows if r[1]<=r[2]<=r[3]]
def stats(values):
 values=sorted(values)
 return {'samples':len(values),'median_us':statistics.median(values),'p95_us':values[(95*len(values)+99)//100-1],'maximum_us':max(values)} if values else None
print(json.dumps({'unique_records':len(rows),'invalid_time_order':len(rows)-len(valid),'queue_to_read':stats([r[2]-r[1] for r in valid]),'hid_send':stats([r[3]-r[2] for r in valid]),'limit':'Measures queue/read/HID submission only; rendering and physical scanout are not measured.'},indent=2))
