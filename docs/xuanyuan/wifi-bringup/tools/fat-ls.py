#!/usr/bin/env python3
"""List a FAT12/16/32 filesystem image (or the first bytes of one), with each entry's start
   cluster, size and computed byte offset. Enough to find the Wi-Fi firmware files on the
   modem/modemfirmware partitions."""
import struct, sys

class Fat:
    def __init__(self, data):
        self.d = data
        bs = data[0:512]
        if bs[510:512] != b"\x55\xaa":
            raise SystemExit("no boot sector signature")
        self.bps = struct.unpack_from("<H", bs, 0x0b)[0]
        self.spc = bs[0x0d]
        self.rsvd = struct.unpack_from("<H", bs, 0x0e)[0]
        self.nfats = bs[0x10]
        self.rootent = struct.unpack_from("<H", bs, 0x11)[0]
        self.totsec16 = struct.unpack_from("<H", bs, 0x13)[0]
        self.totsec32 = struct.unpack_from("<I", bs, 0x20)[0]
        self.fatsz16 = struct.unpack_from("<H", bs, 0x16)[0]
        self.fatsz32 = struct.unpack_from("<I", bs, 0x24)[0]
        self.rootclus = struct.unpack_from("<I", bs, 0x2c)[0]
        self.totsec = self.totsec16 or self.totsec32
        self.fatsz = self.fatsz16 or self.fatsz32
        # cluster count -> type
        rootsecs = (self.rootent * 32 + self.bps - 1) // self.bps
        self.data_start = self.rsvd + self.nfats * self.fatsz + (0 if self.fatsz16 == 0 else rootsecs)
        nclusters = (self.totsec - self.data_start) // self.spc
        self.bits = 32 if nclusters > 65524 else (16 if nclusters >= 4085 else 12)
        self.fat_off = self.rsvd * self.bps
        self.root_off = (self.rsvd + self.nfats * self.fatsz) * self.bps

    def clusters(self):
        return self.totsec

    def next_cluster(self, c):
        if self.bits == 32:
            return struct.unpack_from("<I", self.d, self.fat_off + c * 4)[0] & 0x0FFFFFFF
        if self.bits == 16:
            return struct.unpack_from("<H", self.d, self.fat_off + c * 2)[0]
        off = self.fat_off + c * 3 // 2
        v = struct.unpack_from("<H", self.d, off)[0]
        v = v >> 4 if c & 1 else v & 0xFFF
        return v

    def cluster_off(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.bps

    def chain(self, c, limit=100000):
        out = []
        seen = set()
        while 2 <= c < 0x0FFFFFF0 and c not in seen and len(out) < limit:
            out.append(c)
            seen.add(c)
            c = self.next_cluster(c)
        return out

    def read_chain(self, c, size=None):
        data = b"".join(self.d[self.cluster_off(x): self.cluster_off(x) + self.spc * self.bps] for x in self.chain(c))
        return data[:size] if size else data

    def dir_entries(self, data):
        ents = []
        i = 0
        while i + 32 <= len(data):
            e = data[i:i+32]
            i += 32
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                continue
            attr = e[0x0b]
            if attr & 0x0F == 0x0F:
                continue  # LFN
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            if name in (".", ".."):
                continue
            clus = struct.unpack_from("<H", e, 0x1a)[0] << 16 | struct.unpack_from("<H", e, 0x14)[0]
            size = struct.unpack_from("<I", e, 0x1c)[0]
            full = name + ("." + ext if ext else "")
            ents.append((full, attr, clus, size, self.cluster_off(clus) if clus >= 2 else None))
        return ents

    def walk(self, base=0, prefix=""):
        if base == 0:
            if self.bits == 32:
                data = self.read_chain(self.rootclus)
            else:
                # root dir region in FAT12/16: may extend beyond our partial image
                data = self.d[self.root_off:self.root_off + self.rootent * 32]
        else:
            data = self.read_chain(base)
        for full, attr, clus, size, off in self.dir_entries(data):
            path = prefix + "/" + full
            if attr & 0x10:
                yield path + "/", -1, clus, off
                if clus >= 2:
                    yield from self.walk(clus, path)
            else:
                yield path, size, clus, off

def main(path):
    data = open(path, "rb").read()
    f = Fat(data)
    print(f"# bps={f.bps} spc={f.spc} fats={f.nfats} root_clus={f.rootclus} bits={f.bits} totsec={f.totsec}")
    for p, size, clus, off in f.walk():
        if p.endswith("/"):
            print(f"{p:60s} dir  clus={clus} off={off}")
        else:
            print(f"{p:60s} size={size:>10} clus={clus:>8} off={off}")

if __name__ == "__main__":
    main(sys.argv[1])
