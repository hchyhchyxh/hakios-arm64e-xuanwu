#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
int main(void){
 uint32_t allocation[0x4000/4+8];memset(allocation,0xa5,sizeof(allocation));
 uintptr_t mapped=(uintptr_t)(allocation+4);
 uint64_t table_pa=0xc77f4000,buffer_pa=0xc77e4000;
volatile uint32_t *words=(volatile uint32_t*)mapped;
							for(unsigned i=0;i<0x4000/4;i++)words[i]=0;
							uint64_t entry=(table_pa+0x1000)|3;words[0]=(uint32_t)entry;words[1]=(uint32_t)(entry>>32);
							entry=(table_pa+0x2000)|3;words[0x1000/4]=(uint32_t)entry;words[0x1000/4+1]=(uint32_t)(entry>>32);
							for(unsigned i=0;i<16;i++) {
								entry=(buffer_pa+i*0x1000)|0x60000000000743ULL;
								unsigned at=0x2000/4+(256+i)*2;words[at]=(uint32_t)entry;words[at+1]=(uint32_t)(entry>>32);
							}

 for(unsigned i=0;i<4;i++){assert(allocation[i]==0xa5a5a5a5);assert(allocation[4+0x4000/4+i]==0xa5a5a5a5);}
 assert(fwrite((void*)mapped,1,0x4000,stdout)==0x4000);return 0;
}
