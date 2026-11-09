from pathlib import Path
import struct,subprocess,re,json,hashlib
root=Path('/home/hcy/hakios-arm64e-xuanyuan/xnu-src/isolated/xnu-xnu-12377.121.6/BUILD/obj/DEVELOPMENT_ARM64_BCM2837');out=Path('/mnt/x/air6/xiaomi15ultra-hakios/kernel-build');obj=root/'osfmk/DEVELOPMENT/machine_routines_asm.o';kern=root/'mach.development.bcm2837.unstripped.noctf'
def symbols(p):
 d={}
 for l in subprocess.check_output(['llvm-nm-18',str(p)],text=True).splitlines():
  t=l.split()
  if len(t)==3:
   try:d[t[2]]=int(t[0],16)
   except ValueError:pass
 return d
def section(b,name):
 at=32
 for i in range(struct.unpack_from('<I',b,16)[0]):
  cmd,size=struct.unpack_from('<II',b,at)
  if cmd==25:
   for j in range(struct.unpack_from('<I',b,at+64)[0]):
    s=at+72+j*80
    if b[s:s+16].split(b'\0')[0].decode()==name:return struct.unpack_from('<QQI',b,s+32)
  at+=size
 raise ValueError(name)
ks=symbols(kern);os=symbols(obj);ob=obj.read_bytes();kb=bytearray(kern.read_bytes());oa,sz,off=section(ob,'__copyio_vectors');ka,ksz,koff=section(kb,'__copyio_vectors');base=ks['_copyio_recover_table'];start=koff+base-ka
rel=subprocess.check_output(['llvm-objdump-18','--macho','--reloc',str(obj)],text=True).split('Relocation information (__TEXT,__copyio_vectors)')[1];records=[]
for line in rel.splitlines():
 t=line.split()
 if len(t)>=8 and t[5]=='UNSIGND':
  offset=int(t[0],16);name=t[-1];addend=struct.unpack_from('<q',ob,off+offset)[0];target=ks[name]+addend;value=target-base
  old=struct.unpack_from('<q',kb,start+offset)[0];struct.pack_into('<q',kb,start+offset,value);records.append({'offset':offset,'symbol':name,'addend':addend,'target':hex(target),'old':old,'new':value})
assert len(records)==45,len(records)
for i in range(15):
 a,z,r=struct.unpack_from('<qqq',kb,start+i*24);assert a<=z and z-a<4096
p=out/'mach.xuanyuan.copyio-fixed';p.write_bytes(kb);(out/'copyio-fix-records.json').write_text(json.dumps(records,indent=2));print('45 relative relocations reconstructed; 15 ranges verified;',p.stat().st_size);print('copyout:',records[33:36])
