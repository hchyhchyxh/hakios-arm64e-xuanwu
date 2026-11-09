#include <assert.h>
#include <stdio.h>
#include "xuanyuan-smmu-sync.h"
struct fake {unsigned writes,reads,barriers,wait,clear_after;unsigned offset[4];};
static void barrier(void *c){((struct fake*)c)->barriers++;}
static void write_reg(void *c,unsigned r,uint32_t v){struct fake *f=c;assert(f->writes<4);f->offset[f->writes++]=r;if(r==0x610)assert(v==3);else {assert(r==0x7f0);assert(v==UINT32_MAX);}}
static uint32_t read_reg(void *c,unsigned r){struct fake*f=c;assert(r==0x7f4);return f->reads++<f->clear_after;}
static void delay(void *c,unsigned us){((struct fake*)c)->wait+=us;}
int main(void){
 struct fake f={.clear_after=3};struct xu_sync_state s={0};struct xu_sync_io io={&f,barrier,write_reg,read_reg,delay};
 assert(xu_sync_context(&s,&io,3,100)==0);assert(f.offset[0]==0x610&&f.offset[1]==0x7f0&&f.wait==7&&f.barriers==3);
 f=(struct fake){.clear_after=10000};s=(struct xu_sync_state){0};assert(xu_sync_context(&s,&io,3,100)==-1);assert(f.wait==100&&s.timed_out);
 unsigned writes=f.writes;assert(xu_sync_context(&s,&io,3,100)==-3);assert(f.writes==writes);
 struct fake other={0};struct xu_sync_io other_io={&other,barrier,write_reg,read_reg,delay};
 assert(xu_sync_context(&s,&other_io,4,100)==-3);assert(other.writes==0&&other.reads==0&&other.barriers==0);

 s=(struct xu_sync_state){0};assert(xu_sync_context(&s,&io,256,100)==-2);assert(f.writes==writes);
 assert(xu_sync_context(&s,&io,3,0)==-2);assert(f.writes==writes);
 puts("PASS: invalidation/sync order; completion; bounded timeout; post-timeout writes blocked across contexts; invalid inputs rejected");
}
