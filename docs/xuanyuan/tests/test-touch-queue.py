#!/usr/bin/env python3
"""Compile the actual queue producer against bounded host stubs."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[3]
s=(r/'xnu-src/isolated/xnu-xnu-12377.121.6/bsd/dev/redwood_input.c').read_text()
a=s.index('static void\ntouch_post(');b=s.index('/* when a finger last touched',a)
producer=s[a:b]
# Only isolate the producer; the driver wrapper is outside this slice.
pre=r'''#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define TOUCH_QUEUE 64
#define REDWOOD_TOUCH_DOWN 1
#define REDWOOD_TOUCH_MOVE 2
#define REDWOOD_TOUCH_UP 3
struct redwood_touch { uint64_t time; uint32_t type,finger,x,y; };
static struct redwood_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head,touch_tail;
static int touch_lock;
static uint64_t ticks;
static uint64_t mach_absolute_time(void){return ++ticks;}
static void lck_mtx_lock(int *p){(void)p;}
static void lck_mtx_unlock(int *p){(void)p;}
static void wakeup(void *p){(void)p;}
static bool latency_lines(void){return false;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
static void redwood_tl(const char *s,...){(void)s;}
static void count_fingers(uint32_t t,uint32_t f){(void)t;(void)f;}
'''
main=r'''int main(void){
 touch_post(1,0,10,20,true);
 for(unsigned i=0;i<1000;i++)touch_post(2,0,i,70000,true);
 touch_post(3,0,999,70000,true);
 assert(touch_tail==3 && touch_head==0);
 assert(touch_queue[0].type==1 && touch_queue[1].type==2 && touch_queue[2].type==3);
 assert(touch_queue[1].x==999 && touch_queue[1].y==65535);
 assert(touch_queue[1].time<touch_queue[2].time);
 touch_post(2,0,1,1,true);touch_post(2,1,2,2,true);touch_post(2,0,3,3,true);
 assert(touch_tail==6);
 touch_head=touch_tail=63;
 touch_post(1,0,0,0,true);touch_post(2,0,1,1,true);touch_post(2,0,2,2,true);touch_post(3,0,2,2,true);
 assert(touch_tail==66 && touch_queue[0].type==2 && touch_queue[0].x==2);
 for(unsigned i=0;i<100;i++)touch_post(1,i,0,0,false);
 assert(touch_tail-touch_head==TOUCH_QUEUE);
 return 0;
}'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(pre+producer+main)
 subprocess.run(['gcc','-fsanitize=address,undefined','-g','-o',str(p/'test'),str(p/'test.c')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: actual touch_post producer; coalescing, DOWN/UP, interleaved fingers, clamping, ring wrap and overflow; ASan/UBSan')
