"""Produce an offline SMMUv2 stage-1 candidate, never write hardware."""
def plan(bank,owned_banks,ttbr,asid,ias=36,oas=36):
    if bank not in owned_banks:raise ValueError('context bank ownership not established')
    if ias not in (32,36,39) or oas not in (32,36,40,42,44,48):raise ValueError('unsupported address size')
    if ttbr&4095 or not 0<=ttbr<1<<oas:raise ValueError('TTBR alignment/range')
    if not 0<=asid<=255:raise ValueError('ASID width not validated')
    ips={32:0,36:1,40:2,42:3,44:4,48:5}[oas]
    tcr=(1<<23)|(3<<12)|(64-ias) # TTBR1 off, inner-shareable, non-cacheable walks
    ttbr_value=ttbr|(asid<<48)
    return {'status':'OFFLINE_CANDIDATE_NOT_DEVICE_VERIFIED','bank':bank,'writes_in_order':[
        ['CBA2R',1],['CBAR',0x10000|(3<<8)|(15<<12)],
        ['TCR2',ips|(7<<15)|(1<<4)],['TCR',tcr],['TTBR0',ttbr_value],['TTBR1',0],
        ['MAIR0',0x44],['MAIR1',0]],
        'enable_sctlr':False,'prerequisites':['Prove physical context ownership','Read hardware output-address capability','Reserve table and buffer RAM','Install bounded stream matching','Implement TLB invalidation and sync'],
        'note':'TCR2 includes PASIZE and vendor SEP_UPSTREAM; values remain offline candidates'}


def capabilities(idr2):
    sizes=(32,36,40,42,44,48)
    ipa=idr2&15;pa=(idr2>>4)&15;va=(idr2>>8)&15
    if max(ipa,pa,va)>=len(sizes) or not idr2&(1<<12):
        raise ValueError('unsupported SMMU address capability or 4KiB granule')
    return {'ias':min(sizes[va],39),'oas':sizes[ipa],'physical_bits':sizes[pa]}

if __name__=='__main__':
    import json
    from pathlib import Path
    caps=capabilities(0x5111)
    assert caps=={'ias':36,'oas':36,'physical_bits':36}
    p=plan(3,{3},0xc7800000,1,**{k:caps[k] for k in ('ias','oas')})
    assert dict(p['writes_in_order'])['TCR']&63==28
    assert dict(p['writes_in_order'])['TCR2']&15==1
    p['device_idr2']='0x5111';p['capabilities']=caps
    names=[x[0] for x in p['writes_in_order']]
    assert names.index('TCR')<names.index('TTBR0')<names.index('MAIR0')
    assert not p['enable_sctlr']
    for args in [(82,set(),0xc7800000,1),(3,{3},0xc7800001,1),(3,{3},0xc7800000,256)]:
        try:plan(*args)
        except ValueError:pass
        else:raise AssertionError(args)
    out=Path(__file__).with_suffix('.json');out.write_text(json.dumps(p,indent=2)+'\n')
    print('PASS: live 36-bit capabilities; unowned bank rejected; TTBR/ASID constraints; vendor write ordering; hardware enable omitted')
