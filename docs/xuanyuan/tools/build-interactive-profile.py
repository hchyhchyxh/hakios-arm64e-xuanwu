#!/usr/bin/env python3
"""Build the evidenced Ultra interaction profile without issuing phone commands."""
from pathlib import Path
import argparse,hashlib,json,subprocess,time
r=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--artifacts',type=Path,default=Path('/mnt/x/air6/xiaomi15ultra-hakios'));p.add_argument('--output',type=Path,required=True);p.add_argument('--cpu-query',action='store_true');p.add_argument('--cpus',type=int,choices=(2,4),default=2);p.add_argument('--touch-latency',action='store_true');p.add_argument('--render-scale',type=int,choices=(2,3),default=3);p.add_argument('--seconds',type=int,default=900);o=p.parse_args()
if not 60<=o.seconds<=1800:p.error('seconds must be 60..1800')
c=json.loads((r/'docs/xuanyuan/interactive-profile.json').read_text());root=o.artifacts/'phone-ios26-test'/c['root_filename']
assert hashlib.sha256(root.read_bytes()).hexdigest()==c['root_sha256'],'interaction root does not match the checked profile'
args=[f'cpus={o.cpus}' if x.startswith('cpus=') else x for x in c['arguments']]+[f'loader_return_after={o.seconds}',f'epoch={int(time.time())}']
if o.cpu_query:args+=['dadacpu=1']
if o.touch_latency:args+=['touch_lat=1']
if o.render_scale!=3:args+=[f'fbscale={o.render_scale}']
subprocess.run(['python3',str(r/'base/xnuloader/mkpayload.py'),'-o',str(o.output),'kernel='+str(o.artifacts/'kernel-build/mach.xuanyuan.copyio-fixed'),'ramdisk='+str(root),'--cmdline',' '.join(args)],check=True)
print('Built interaction profile; CPU performance-level writes and governor remain disabled; no device command issued')
