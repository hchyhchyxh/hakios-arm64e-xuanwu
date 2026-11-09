#!/usr/bin/env python3
"""Summarize one captured loader log without mistaking duplicate log copies for crashes."""
import argparse,json,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('logs',nargs='+',type=Path);o=p.parse_args()
lines={}
for path in o.logs:
 for line in path.read_text(errors='replace').splitlines():lines[line]=None
s='\n'.join(lines)
def unique(pattern):return list(dict.fromkeys(re.findall(pattern,s)))
windows=unique(r'(\d+) frames in (\d+) ms\s*\((\d+) a second\), (\d+) us each in the driver')
result={'sources':[str(path) for path in o.logs],'caenv':unique(r'redwood(?:26)?: caenv: ([^\n]+)'),
'compositor_crashes':unique(r'hakios: crash backboardd\[(\d+)\] signal (\d+)'),
'cpu_observations':unique(r'dcpu: [^\n]+'),
'cpu_start_requests':unique(r'hakios: psci CPU_ON cpu (\d+) \(MPIDR (0x[0-9a-f]+)\)'),
'push_surface_assertion': 'Assertion failed: (found != slist->end ()), function push_surface' in s,
'preferences_visible':'app<com.apple.Preferences((null))>, running-active-Visible' in s,
'frame_windows':[dict(zip(('frames','duration_ms','fps','driver_us'),map(int,w))) for w in windows],
'last_touch':(unique(r'dtouch: so far [^\n]+') or [None])[-1],
'limitation':'No crash in a capture is not proof of stability; compare equivalent physical interactions.'}
print(json.dumps(result,indent=2))
