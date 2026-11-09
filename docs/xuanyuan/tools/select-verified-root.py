from pathlib import Path
import struct,hashlib,json
base=Path('/mnt/x/air6/xiaomi15ultra-hakios/phone-ios26-test');data=(base/'ios26-unlock-candidate.bin').read_bytes();magic,total=struct.unpack_from('<II',data);assert magic==0x50554e58 and total==len(data)-8
at=8;root=None
while at<len(data):
 name,size,res=struct.unpack_from('<16sII',data,at);at+=24;name=name.rstrip(b'\0')
 if not name:break
 assert at+size<=len(data)
 if name==b'ramdisk':root=data[at:at+size]
 at+=size+(-size%16)
assert root and len(root)==4194304
current=Path('/home/hcy/hakios-arm64e-xuanyuan/userland/userland/root.apfs').read_bytes();assert root==current,'known working payload and source image differ'
(base/'root-verified-seed.apfs').write_bytes(root)
r=Path('/home/hcy/hakios-arm64e-xuanyuan/docs/xuanyuan/android-iommu-reference');(r/'root-image-selection.json').write_text(json.dumps({'known_working_payload':'ios26-unlock-candidate.bin','verified_root_sha256':hashlib.sha256(root).hexdigest(),'verified_root_bytes':len(root),'matches_current_userland_build':True,'stale_host_root_sha256':hashlib.sha256((base/'root.apfs').read_bytes()).hexdigest(),'stale_host_root_used_in_failed_regression':True,'manifest_regression_not_attributed_to_kernel':True,'selected_image':'root-verified-seed.apfs'},indent=2)+'\n')
print('PASS: known working payload root equals current userland image; extracted as root-verified-seed.apfs')
