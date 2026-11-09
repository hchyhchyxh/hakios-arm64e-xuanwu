#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "xuanyuan-gpi-tre.h"
int main(void) {
 struct xu_gpi_tre t;
 assert(sizeof(t)==16);
 assert(xu_gpi_spi_config(&t,7,8)==0);
 const uint32_t config[4]={0x03000004,0,0x00070008,0x00220001};
 assert(memcmp(t.word,config,16)==0);
 assert(xu_gpi_spi_config_delays(&t,7,8,30,0)==0);
 assert(t.word[1]==0x1e00 && t.word[2]==0x70008);
 assert(xu_gpi_spi_config_delays(&t,7,8,256,0)==-1);
 assert(xu_gpi_spi_go(&t,3,0,4)==0);
 const uint32_t duplex[4]={3,0,4,0x00200801};assert(memcmp(t.word,duplex,16)==0);
 assert(xu_gpi_dma(&t,UINT64_C(0x123456789),64,1)==0);
 const uint32_t dma[4]={0x23456789,1,64,0x00100200};assert(memcmp(t.word,dma,16)==0);
 assert(xu_gpi_dma(&t,UINT64_MAX,2,1)==-1);
 assert(xu_gpi_dma(&t,0x100000,0,1)==-1);
 assert(xu_gpi_spi_config(&t,16,8)==-1);
 assert(xu_gpi_spi_config(&t,7,0)==-1);
 assert(xu_gpi_spi_go(&t,4,0,4)==-1);
 assert(xu_gpi_spi_go(&t,3,0,0x1000000)==-1);
 puts("PASS: 16-byte TRE ABI; captured clock config; duplex/DMA golden vectors; invalid sizes and overflow rejected");
}
