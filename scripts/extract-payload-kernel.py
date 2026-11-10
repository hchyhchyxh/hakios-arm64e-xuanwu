#!/usr/bin/env python3
"""Extract the kernel entry from an XNUP payload (header: u32 magic, u32 size; entries:
   char name[16], u32 size, u32 reserved, data padded to 16)."""
import struct, sys, pathlib

src = sys.argv[1]
dst = sys.argv[2]
data = pathlib.Path(src).read_bytes()
magic, size = struct.unpack_from('<II', data, 0)
assert magic == 0x50554E58, f"not an XNUP payload: {magic:#x}"
off = 8
found = {}
while off + 24 <= len(data):
    name = data[off:off+16].split(b'\0')[0].decode()
    esize, _res = struct.unpack_from('<II', data, off+16)
    body = data[off+24:off+24+esize]
    if name == '' and esize == 0:
        break
    found[name] = body
    off += 24 + esize + (-esize % 16)
print("entries:", {k: len(v) for k, v in found.items()})
k = found.get('kernel')
assert k is not None
pathlib.Path(dst).write_bytes(k)
print(f"kernel written: {dst} {len(k)} bytes")
