#!/usr/bin/env python3
"""Capture bounded live loader logs; never flash, reboot, or change partitions."""
import argparse,subprocess,time,json
from pathlib import Path

def command(executable,args,timeout=8):
    try:
        p=subprocess.run([executable,*args],capture_output=True,text=True,encoding='utf-8',errors='backslashreplace',timeout=timeout)
        return p.returncode,p.stdout+p.stderr
    except subprocess.TimeoutExpired:
        return 124,'HOST_COMMAND_TIMEOUT'

def main():
    a=argparse.ArgumentParser();a.add_argument('--fastboot',required=True);a.add_argument('--out',required=True);a.add_argument('--seconds',type=int,default=120);a.add_argument('--interval',type=float,default=3);a.add_argument('--quiet',action='store_true');o=a.parse_args()
    if not 1<=o.seconds<=600:a.error('seconds must be 1..600')
    if not 0.05<=o.interval<=30:a.error('interval must be .05..30')
    out=Path(o.out);out.mkdir(parents=True,exist_ok=True);start=time.monotonic();records=[]
    while time.monotonic()-start<o.seconds:
        elapsed=round(time.monotonic()-start,1);code,result=command(o.fastboot,['getvar','result'])
        record={'seconds':elapsed,'exit':code,'result':result.strip()}
        if not o.quiet or not records or result.strip()!=records[-1]['result']:print(json.dumps(record),flush=True)
        if code==0:
            status,_=command(o.fastboot,['oem','log'])
            if status==0:
                status,detail=command(o.fastboot,['get_staged',str(out/f'loader-{len(records):03d}.log')],20)
                record['log_exit']=status
            if 'result: returned' in result:
                records.append(record);break
        records.append(record)
        if len(records)>=3 and all(r['exit']!=0 for r in records[-3:]):break
        time.sleep(o.interval)
    (out/'poll-status.json').write_text(json.dumps(records,indent=2)+'\n')
    return 0 if records and 'result: returned' in records[-1]['result'] else 1
if __name__=='__main__':raise SystemExit(main())
