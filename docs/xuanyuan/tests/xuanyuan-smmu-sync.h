/* Host-testable SMMUv2 context synchronization candidate. No concrete MMIO backend. */
#ifndef XU_SMMU_SYNC_H
#define XU_SMMU_SYNC_H
#include <stdint.h>
/* One state per SMMU, shared by every context and serialized by the caller. */
struct xu_sync_state { unsigned timed_out; };
struct xu_sync_io {
 void *cookie;
 void (*barrier)(void *);
 void (*write)(void *,unsigned,uint32_t);
 uint32_t (*read)(void *,unsigned);
 void (*delay_us)(void *,unsigned);
};
/* Caller must serialize access, share SMMU state, and prove context ownership. */
static inline int xu_sync_context(struct xu_sync_state *s,const struct xu_sync_io *io,unsigned asid,unsigned budget_us)
{
 if (!s||!io||!io->barrier||!io->write||!io->read||!io->delay_us||asid>255||!budget_us) return -2;
 if (s->timed_out) return -3;
 io->barrier(io->cookie);
 io->write(io->cookie,0x610,asid);
 io->barrier(io->cookie);
 io->write(io->cookie,0x7f0,UINT32_MAX);
 unsigned elapsed=0,increment=1;
 for(;;) {
  if(!(io->read(io->cookie,0x7f4)&1u)) {io->barrier(io->cookie);return 0;}
  if(elapsed>=budget_us) {s->timed_out=1;return -1;}
  unsigned wait=increment;
  if(wait>budget_us-elapsed)wait=budget_us-elapsed;
  io->delay_us(io->cookie,wait);elapsed+=wait;
  if(increment<128)increment*=2;
 }
}
#endif
