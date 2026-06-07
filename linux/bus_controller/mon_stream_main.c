/* mon_stream — enable CMD_MON_STREAM on the core1 app engine (0xFB) and count the
 * periodic report cycles (one OP_MON_END per cycle), then disable. PASS = >=4 cycles. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "controller.h"
#include "usb_link.h"
#define APPCORE 0xFB
#define CMD_MON_STREAM 0x0202
#define OP_MON_END 0x3F
static volatile int g_stop=0; static void on_sigint(int s){(void)s;g_stop=1;}
static uint64_t mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
typedef struct { int cycles, sys, mem; } ctx_t;
static void on_raw(void*u,uint8_t a,uint16_t op,const uint8_t*p,uint16_t l){(void)a;(void)p;(void)l;
  ctx_t*c=(ctx_t*)u; if(op==OP_MON_END){c->cycles++; printf("  cycle %d (END)\n",c->cycles);} else if(op==0x30)c->sys++; else if(op==0x32)c->mem++; }
int main(int argc,char**argv){
  signal(SIGINT,on_sigint); const char*dev=(argc>1)?argv[1]:NULL;
  usb_link_cfg_t cfg={.device=dev,.baud=115200,.assert_dtr=1,.reconnect_ms_min=200,.reconnect_ms_max=1000};
  link_endpoint_t*ep=usb_link_create(&cfg,NULL,NULL,NULL); if(!ep)return 1;
  controller_t*ctrl=controller_create(ep); if(!ctrl)return 1;
  ctx_t ctx; memset(&ctx,0,sizeof ctx); controller_set_raw_cb(ctrl,on_raw,&ctx);
  int sent=0; uint64_t t0=0;
  while(!g_stop){ controller_poll(ctrl);
    if(controller_is_operational(ctrl)&&!sent){ uint8_t a[5]={1,0xF4,0x01,0,0}; /*enable,period=500ms,mask=0*/
      printf("STREAM on (500ms) -> 0xFB\n"); controller_send_shell_to(ctrl,APPCORE,CMD_MON_STREAM,a,5,NULL,NULL); sent=1; t0=mono(); }
    if(sent && mono()-t0>3500) break;
    struct timespec t={0,3*1000*1000}; nanosleep(&t,NULL);
  }
  uint8_t off[5]={0,0,0,0,0}; controller_send_shell_to(ctrl,APPCORE,CMD_MON_STREAM,off,5,NULL,NULL);
  for(int i=0;i<50;i++){controller_poll(ctrl);struct timespec t={0,3*1000*1000};nanosleep(&t,NULL);}
  int ok=ctx.cycles>=4 && ctx.sys>=4 && ctx.mem>=4;
  printf("\n[mon_stream] %s (cycles=%d sys=%d mem=%d)\n",ok?"PASS":"FAIL",ctx.cycles,ctx.sys,ctx.mem);
  controller_destroy(ctrl); link_close(ep); return ok?0:1;
}
