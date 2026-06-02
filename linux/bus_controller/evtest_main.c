// evtest_main.c — verify the FFI event seam (controller_submit_command_ev +
// controller_drain) on the real bus, before the LuaJIT wrapper rides it.
//
//   build: make evtest      run: ./evtest <BC /dev/ttyACMx> [roster.conf]
//
// Brings up + enables the sweep, submits N echoes via the *_ev path (no callback),
// and collects their completions purely by DRAINING CTRL_EV_CMD_DONE events. Proves
// the queue delivers results, in order, keyed by handle — the wrapper's contract.

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "usb_link.h"
#include "controller.h"

#define CMD_ECHO   0x0001
#define SLAVE_ADDR 1
#define N          5

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }
static uint64_t mono_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000u + (uint64_t)(t.tv_nsec/1000000u); }

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev = (argc>1 && argv[1][0]!='-') ? argv[1] : NULL;
    const char *rp  = (argc>2) ? argv[2] : "rosters/one_slave.conf";
    roster_t roster; char err[128]={0};
    if (roster_load_file(rp,&roster,err,sizeof err)!=0){ fprintf(stderr,"roster: %s\n",err); return 1; }
    usb_link_cfg_t cfg = { .device=dev,.baud=115200,.assert_dtr=1,.reconnect_ms_min=200,.reconnect_ms_max=1000 };
    link_endpoint_t *ep = usb_link_create(&cfg,NULL,NULL,NULL);
    if(!ep){ fprintf(stderr,"usb_link_create\n"); return 1; }
    controller_t *c = controller_create(ep);
    controller_attach_roster(c,&roster);

    printf("[evtest] bring up...\n");
    uint64_t dl = mono_ms()+8000; int up=0;
    while(!g_stop && mono_ms()<dl){
        controller_poll(c);
        if(controller_prov_state(c)==PROV_DONE){ controller_set_poll_enable(c,1); up=1; break; }
        struct timespec ts={0,3*1000*1000}; nanosleep(&ts,NULL);
    }
    if(!up){ fprintf(stderr,"[evtest] never provisioned\n"); return 1; }
    { struct timespec ts={1,500*1000*1000}; nanosleep(&ts,NULL); controller_poll(c); }

    // Submit N echoes via the event path; remember handles for FIFO check.
    uint8_t args[4] = { 2,0,'e','v' };
    uint32_t h[N]; for(int i=0;i<N;i++){ h[i]=controller_submit_command_ev(c,SLAVE_ADDR,CMD_ECHO,args,4,1000);
        printf("[evtest] submit #%d -> handle=%u\n",i,h[i]); }

    int done=0, ok=0, order_ok=1, next=0;
    uint64_t deadline = mono_ms()+10000;
    while(!g_stop && done<N && mono_ms()<deadline){
        controller_poll(c);
        ctrl_event_t ev;
        while(controller_drain(c,&ev)){
            if(ev.kind==CTRL_EV_CMD_DONE){
                int echo_ok = (ev.status==0 && ev.data_len>=4 && ev.data[0]==2 && ev.data[2]=='e' && ev.data[3]=='v');
                int fifo = (ev.handle==h[next]);
                printf("  drain CMD_DONE h=%u status=%u echo=%s%s\n", ev.handle, ev.status,
                       echo_ok?"OK":"BAD", fifo?"":" OUT-OF-ORDER");
                if(!fifo) order_ok=0; else next++;
                if(echo_ok) ok++;
                done++;
            } else {
                printf("  drain other-event kind=%u addr=%u aux=%u\n", ev.kind, ev.addr, ev.aux);
            }
        }
        struct timespec ts={0,2*1000*1000}; nanosleep(&ts,NULL);
    }
    printf("[evtest] %s  (%d/%d CMD_DONE drained, ok=%d, order_ok=%d)\n",
           (done==N&&ok==N&&order_ok)?"PASS":"FAIL", done, N, ok, order_ok);
    controller_destroy(c); link_close(ep);
    return (done==N&&ok==N&&order_ok)?0:1;
}
