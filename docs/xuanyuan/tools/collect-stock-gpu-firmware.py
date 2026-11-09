#!/usr/bin/env python3
"""Read stock Adreno firmware through an already-authorized root ADB connection."""
import argparse,hashlib,json,re,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--adb',required=True);p.add_argument('--serial',required=True);p.add_argument('--output',type=Path,required=True);o=p.parse_args()
def adb(args,binary=False):
 v=subprocess.run([o.adb,'-s',o.serial,*args],capture_output=True,timeout=45,check=True)
 return v.stdout if binary else v.stdout.decode('utf-8','replace')
def root(command,binary=False):return adb(['exec-out','su','-c',command],binary)
assert adb(['get-state']).strip()=='device','Android ADB is not ready'
assert root('id').startswith('uid=0'),'root Shell authorization required'
search='find -L /vendor/firmware /vendor/firmware_mnt/image /odm/firmware /system/etc/firmware -type f -name "gen80000*" 2>/dev/null; true'
paths=sorted(set(root(search).splitlines()));o.output.mkdir(parents=True,exist_ok=True);records=[]
for path in paths:
 if not re.fullmatch(r'/(?:vendor/(?:firmware|firmware_mnt/image)|odm/firmware|system/etc/firmware)/[A-Za-z0-9_./+-]*gen80000[A-Za-z0-9_.+-]*',path):continue
 name=Path(path).name;dest=o.output/(hashlib.sha256(path.encode()).hexdigest()[:10]+'-'+name)
 data=root('cat '+shlex.quote(path),True)
 if not data:raise RuntimeError('empty firmware: '+path)
 digest=hashlib.sha256(data).hexdigest();remote=root('sha256sum '+shlex.quote(path)).split()[0]
 if digest!=remote:raise RuntimeError('hash mismatch: '+path)
 if dest.exists() and dest.read_bytes()!=data:raise RuntimeError('existing local firmware differs: '+str(dest))
 dest.write_bytes(data);records.append({'remote_path':path,'local_file':dest.name,'size':len(data),'sha256':digest,'remote_hash_matches':True})
(o.output/'manifest.json').write_text(json.dumps({'files':records,'scope':'Read-only stock firmware capture; no compatibility claim'},indent=2)+'\n')
if not records:raise SystemExit('No matching stock firmware found; inspect stock firmware names before loading')
print(json.dumps({'captured_files':len(records),'manifest':str(o.output/'manifest.json')},indent=2))
