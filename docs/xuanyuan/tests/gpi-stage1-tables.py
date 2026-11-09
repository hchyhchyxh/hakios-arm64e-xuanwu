"""Offline-only 36-bit IOVA/4KiB granule stage-1 table candidate.
No SMMU registers are written. Context ownership/TCR/MAIR need device validation.
"""
import struct,json
from pathlib import Path
PAGE=4096
IOVA_START=0x100000
IOVA_END=0x200000
MASK=0x0000fffffffff000

def build(table_pa,buffer_pa,size,physical_bits=36):
    if table_pa%PAGE or buffer_pa%PAGE or size%PAGE or not 0<size<=IOVA_END-IOVA_START:
        raise ValueError('page alignment or DMA window violation')
    if physical_bits not in (32,36,40,42,44,48):raise ValueError('unsupported physical address width')
    if min(table_pa,buffer_pa)<0 or max(table_pa+3*PAGE,buffer_pa+size)>1<<physical_bits:
        raise ValueError('physical capability range violation')
    if table_pa<buffer_pa+size and buffer_pa<table_pa+3*PAGE:
        raise ValueError('page tables overlap DMA buffers')
    tables=bytearray(3*PAGE)
    struct.pack_into('<Q',tables,0,(table_pa+PAGE)|3)
    struct.pack_into('<Q',tables,PAGE,(table_pa+2*PAGE)|3)
    # Normal non-cacheable attribute index 0, inner-shareable, AF, RW, unprivileged, XN.
    flags=3|(1<<6)|(3<<8)|(1<<10)|(3<<53)
    for offset in range(0,size,PAGE):
        index=((IOVA_START+offset)>>12)&511
        struct.pack_into('<Q',tables,2*PAGE+index*8,(buffer_pa+offset)|flags)
    return bytes(tables)

def verify_layout(tables,table_pa,buffer_pa,size):
    if len(tables)!=3*PAGE:raise ValueError('wrong table allocation size')
    if struct.unpack_from('<Q',tables,0)[0]!=(table_pa+PAGE)|3:raise ValueError('invalid root table descriptor')
    if struct.unpack_from('<Q',tables,PAGE)[0]!=(table_pa+2*PAGE)|3:raise ValueError('invalid level-two descriptor')
    for offset in range(0,size,PAGE):
        at=2*PAGE+(((IOVA_START+offset)>>12)&511)*8
        entry=struct.unpack_from('<Q',tables,at)[0]
        if entry&MASK != buffer_pa+offset:raise ValueError('unexpected physical mapping')
        required=3|(1<<6)|(3<<8)|(1<<10)|(3<<53)
        if entry&~MASK != required:raise ValueError('invalid page access attributes')
    for page in range(512):
        va=page*PAGE
        if not IOVA_START<=va<IOVA_START+size and struct.unpack_from('<Q',tables,2*PAGE+page*8)[0]:
            raise ValueError('mapping escapes DMA aperture')
    return True


def walk(tables,table_pa,iova,input_bits=36):
    if input_bits not in (32,36,39):raise ValueError('unsupported input address width')
    if not 0<=iova<1<<input_bits:return None
    current=table_pa
    for shift in (30,21,12):
        at=current-table_pa+((iova>>shift)&511)*8
        if not 0<=at<=len(tables)-8:return None
        entry=struct.unpack_from('<Q',tables,at)[0]
        if entry&3!=3:return None
        if shift==12:return (entry&MASK)|(iova&4095)
        current=entry&MASK
    return None

if __name__=='__main__':
    import random,tempfile
    table_pa=0xc7800000;buffer_pa=0xc77e0000;size=0x10000
    tables=build(table_pa,buffer_pa,size)
    assert verify_layout(tables,table_pa,buffer_pa,size)
    corrupted=bytearray(tables);corrupted[2*PAGE+256*8] ^= 0x80
    try:verify_layout(corrupted,table_pa,buffer_pa,size)
    except ValueError:pass
    else:raise AssertionError('read-only corruption accepted')
    rng=random.Random(8750)
    for offset in [0,4095,4096,size-1]+[rng.randrange(size) for _ in range(1000)]:
        assert walk(tables,table_pa,IOVA_START+offset)==buffer_pa+offset
    for iova in [0,IOVA_START-1,IOVA_START+size,IOVA_END-1,IOVA_END,1<<39]:
        assert walk(tables,table_pa,iova) is None
    for args in [(table_pa+1,buffer_pa,size),(table_pa,buffer_pa,0x101000),(buffer_pa,buffer_pa,size),(1<<36,buffer_pa,size),(table_pa,(1<<36)-PAGE,size)]:
        try:build(*args)
        except ValueError:pass
        else:raise AssertionError(args)
    out=Path(__file__).resolve().parent
    (out/'gpi-stage1-tables-candidate.bin').write_bytes(tables)
    result={'status':'OFFLINE_PASS_DEVICE_NOT_PROGRAMMED','iova_window':[hex(IOVA_START),hex(IOVA_END)],'mapped_bytes':size,'physical_base':hex(buffer_pa),'table_base':hex(table_pa),'translation_checks':1004,'out_of_range_checks':6,'invalid_layout_checks':5,'input_bits':36,'physical_bits':36,'caveats':['Actual SMMU context ownership, TCR and MAIR not configured','No DMA or register writes performed']}
    (out/'gpi-stage1-tests.json').write_text(json.dumps(result,indent=2)+'\n')
    assert (out/'gpi-stage1-tables-candidate.bin').read_bytes()==tables
    print('PASS: 1004 translated offsets; 6 unmapped boundaries; 5 invalid layouts rejected; no device writes')
