#!/usr/bin/env python3
"""Inspect the firmware block format and address ranges used by the current Ultra test driver."""
import argparse,hashlib,json,struct
from pathlib import Path
RANGES=[('ITCM',0,0x4000),('DTCM',0x10004000,0x4000),('ICACHE',0x4000,0xffc000),('DUMMY',0x60000000,0x4000),('DEBUG',0x60400000,0x7000)]
def inspect(data):
 if not data:raise ValueError('empty firmware')
 at=0;blocks=[]
 while at<len(data):
  if len(data)-at<16:raise ValueError('truncated block header')
  address,size,kind,value=struct.unpack_from('<4I',data,at);offset=at;at+=16
  if size>len(data)-at:raise ValueError('truncated block payload')
  region=None
  if size:
   if address+size>0x100000000:raise ValueError('address overflow')
   region=next((name for name,start,length in RANGES if start<=address and address+size<=start+length),None)
   if region is None:raise ValueError('payload outside current driver memory ranges')
   if region in ('ITCM','DTCM') and (address%4 or size%4):raise ValueError('unaligned TCM payload')
  blocks.append({'offset':offset,'address':hex(address),'size':size,'type':kind,'value':value,'region':region});at+=size
 return {'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'blocks':blocks,'scope':'Format/range validation only; authenticity and hardware compatibility are not verified.'}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('image',type=Path);o=p.parse_args()
 try:print(json.dumps(inspect(o.image.read_bytes()),indent=2))
 except ValueError as e:p.exit(1,str(e)+'\n')
