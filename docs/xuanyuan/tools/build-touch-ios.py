#!/usr/bin/env python3
"""Build the current Ultra touch test payload; does not issue device commands."""
import argparse,hashlib,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parents[3]
a=argparse.ArgumentParser();a.add_argument('--artifacts',type=Path,default=Path('/mnt/x/air6/xiaomi15ultra-hakios'));a.add_argument('--seconds',type=int,default=900);a.add_argument('--cpus',type=int,choices=(1,2),default=1);a.add_argument('--fb-threads',type=int,choices=(1,2,3),default=3);a.add_argument('--output',type=Path);o=a.parse_args()
if not 60<=o.seconds<=1800:a.error('seconds must be 60..1800')
b=o.artifacts/'phone-ios26-test';root=b/'root-verified-seed.apfs'
assert hashlib.sha256(root.read_bytes()).hexdigest()=='17784f814957e6f75b40905bb4f46309f2cb986a15b83cd83008c38048c8e75a','verified root image changed'
output=o.output or b/'ios26-small-font-touch.bin'
args=f'cpus={o.cpus} fb_threads={o.fb_threads} bootcpu=2 -v loader_return_after={o.seconds} vm_compression_limit=131072 console_zoom=1 console_pages=0 xuanyuantouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 epoch={int(time.time())}'
subprocess.run(['python3',str(r/'base/xnuloader/mkpayload.py'),'-o',str(output),'kernel='+str(o.artifacts/'kernel-build/mach.xuanyuan.copyio-fixed'),'ramdisk='+str(root),'--cmdline',args],check=True)
print('Small-font touch payload built; no device command executed')
