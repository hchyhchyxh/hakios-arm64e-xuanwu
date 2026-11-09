#include "xuanyuan-smmu-mmio.h"
void zero(uintptr_t a){xu_smmu_write32(a,0);}
void all_ones(uintptr_t a){xu_smmu_write32(a,UINT32_MAX);}
void variable(uintptr_t a,uint32_t v){xu_smmu_write32(a,v);}
void baseline_zero(uintptr_t a){*(volatile uint32_t*)a=0;}
