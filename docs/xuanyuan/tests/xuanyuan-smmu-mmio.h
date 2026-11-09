/* Qualcomm trapped MMIO must not use WZR/XZR as a store operand. */
#ifndef XU_SMMU_MMIO_H
#define XU_SMMU_MMIO_H
#include <stdint.h>
static inline void xu_smmu_write32(uintptr_t address,uint32_t value)
{
 __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(address) : "memory");
}
#endif
