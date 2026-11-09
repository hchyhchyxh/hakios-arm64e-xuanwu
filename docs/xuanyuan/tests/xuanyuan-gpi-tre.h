/* SPDX-License-Identifier: GPL-2.0-only
 * GPI SPI transfer elements; layout verified against Linux qcom/gpi.c.
 * Addresses passed here are DMA/IOMMU addresses, never kernel virtual addresses.
 */
#ifndef XUANYUAN_GPI_TRE_H
#define XUANYUAN_GPI_TRE_H
#include <stdint.h>
struct xu_gpi_tre { uint32_t word[4]; };
static inline int xu_gpi_spi_config(struct xu_gpi_tre *t, uint32_t source, uint32_t divider)
{
 if (!t || source>15 || !divider || divider>4095) return -1;
 *t=(struct xu_gpi_tre){{4u | (1u<<24) | (1u<<25),0,divider | (source<<16),(0x22u<<16)|1u}};
 return 0;
}
static inline int xu_gpi_spi_config_delays(struct xu_gpi_tre *t,uint32_t source,uint32_t divider,uint32_t cs_delay,uint32_t word_delay)
{
 if(cs_delay>255 || word_delay>255 || xu_gpi_spi_config(t,source,divider))return -1;
 t->word[1]=(cs_delay<<8)|word_delay;
 return 0;
}
static inline int xu_gpi_spi_go(struct xu_gpi_tre *t, uint32_t command, uint32_t cs, uint32_t rx_length)
{
 if (!t || command<1 || command>3 || cs>7 || rx_length>0xffffffu) return -1;
 uint32_t flags=command==1 ? 1u : command==2 ? (1u<<8)|(1u<<11) : 1u|(1u<<11);
 *t=(struct xu_gpi_tre){{command|(cs<<8),0,rx_length,(0x20u<<16)|flags}};
 return 0;
}
static inline int xu_gpi_dma(struct xu_gpi_tre *t, uint64_t dma_address, uint32_t length, int transmit)
{
 if (!t || !length || length>0xffffffu || dma_address>UINT64_MAX-(length-1u)) return -1;
 *t=(struct xu_gpi_tre){{(uint32_t)dma_address,(uint32_t)(dma_address>>32),length,(0x10u<<16)|(transmit ? 1u<<9 : 0)}};
 return 0;
}
#endif
