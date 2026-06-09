/* mon_pio_b — exercise the SAMD21 PIO-mode gpio interlock (PIO-b).
 *
 * Loads a DSL into the interlock_cfg record, enters PIO, then uses the fact that
 * output channels keep INEN (read-back) to drive the watched input via OLAT — no
 * jumper needed. Verifies: parse, trip->INT_FLAGS, out_err override of OLAT,
 * manual-reset when the fault clears, and re-trip when the fault persists.
 *
 * DSL:  gil;cfg[D1:in,D2:out];watch[D1:eq:1];out_err[D2:0]
 *   watch OK while D1(CH1)==1; on trip force D2(CH2)=0 and assert INT (D10).
 *
 *   ./mon_pio_b <BC> [roster] [addr_hex]
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "usb_link.h"
#include "controller.h"

#define APPCORE             0xFB
#define CMD_I2C_WRITE       0x010D
#define CMD_I2C_WRITE_READ  0x010F
#define REG_MODE     0x02
#define REG_INTFLAGS 0x04
#define MODE_PIO     1
#define REG_IODIR    0x10
#define REG_GPIO     0x13
#define REG_OLAT     0x14
#define REG_ILSTAT   0x15
#define REG_ILSTATE  0x16
#define REG_REC_SEL  0x40
#define REG_REC_LEN  0x41
#define REG_REC_DATA 0x43
#define REG_REC_CTRL 0x44
#define REG_STORE_STAT 0x45
#define REC_INTERLOCK_CFG 4

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void nap_ms(unsigned ms){ struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }

typedef struct { int done; uint8_t status; uint8_t buf[256]; uint16_t len; } reply_t;
static reply_t g_reply;
static void on_reply(void *u, uint16_t rid, uint8_t st, const uint8_t *r, uint16_t len){
    (void)rid; reply_t *s=(reply_t*)u; s->status=st; s->len=(len>sizeof s->buf)?(uint16_t)sizeof s->buf:len;
    if(r&&s->len) memcpy(s->buf,r,s->len); s->done=1;
}
static int call_to(controller_t *c, uint8_t addr, uint16_t cmd, const uint8_t *args, uint16_t alen, reply_t *out){
    g_reply.done=0; uint16_t rid=controller_send_shell_to(c,addr,cmd,args,alen,on_reply,&g_reply);
    if(rid==0xFFFF) return -1; uint64_t dl=mono_ms()+4000;
    while(!g_reply.done && mono_ms()<dl && !g_stop){ controller_poll(c); nap_ms(2);} *out=g_reply; return g_reply.done?0:-1;
}
static uint8_t g_addr = 0x55;
static int reg_read(controller_t *c, uint8_t reg, uint8_t *dst, uint8_t n){
    uint8_t a[3]={g_addr,n,reg}; reply_t r;
    if(call_to(c,APPCORE,CMD_I2C_WRITE_READ,a,3,&r)!=0||r.status!=0||r.len<n) return -1; memcpy(dst,r.buf,n); return 0;
}
static int reg_write(controller_t *c, uint8_t reg, uint8_t val){
    uint8_t a[3]={g_addr,reg,val}; reply_t r; return (call_to(c,APPCORE,CMD_I2C_WRITE,a,3,&r)!=0||r.status!=0)?-1:0;
}
static int store_write(controller_t *c, uint8_t rec, const uint8_t *data, uint8_t len){
    if(reg_write(c,REG_REC_SEL,rec)!=0) return -1;        /* select, off->0 */
    if(reg_write(c,REG_REC_LEN,len)!=0) return -1;
    for(uint8_t off=0; off<len; ){                        /* chunk: reg+data must be <= 64 (HIL_I2C_MAX_LEN) */
        uint8_t chunk = (uint8_t)(len-off); if(chunk>32) chunk=32;
        uint8_t a[2+32]; a[0]=g_addr; a[1]=REG_REC_DATA; memcpy(&a[2],data+off,chunk);  /* data port: off auto-advances */
        reply_t r; if(call_to(c,APPCORE,CMD_I2C_WRITE,a,(uint16_t)(2+chunk),&r)!=0||r.status!=0) return -1;
        off=(uint8_t)(off+chunk);
    }
    if(reg_write(c,REG_REC_CTRL,0xC0)!=0) return -1;                       /* commit */
    for(int i=0;i<200;i++){ uint8_t st; if(reg_read(c,REG_STORE_STAT,&st,1)==0 && !(st&0x01)) return (st&0x02)?-2:0; nap_ms(5); }
    return -3;
}
static void store_dump(controller_t *c, uint8_t rec, uint8_t len){       /* read-back diagnostic */
    if(reg_write(c,REG_REC_SEL,rec)!=0) return;
    uint8_t a[3]={g_addr,len,REG_REC_DATA}; reply_t r;
    if(call_to(c,APPCORE,CMD_I2C_WRITE_READ,a,3,&r)!=0||r.status!=0){ printf("    (readback failed)\n"); return; }
    printf("    readback[%u]: \"%.*s\"\n", r.len, (int)r.len, (const char*)r.buf);
}

static int g_pass = 1;
static uint8_t rd(controller_t *c, uint8_t reg){ uint8_t v=0xFF; reg_read(c,reg,&v,1); return v; }
static void expect(const char *what, uint8_t got, uint8_t want){
    int ok=(got==want); if(!ok) g_pass=0;
    if(ok) printf("    %-26s = 0x%02X  ok\n", what, got);
    else   printf("    %-26s = 0x%02X  MISMATCH (want 0x%02X)\n", what, got, want);
}

int main(int argc, char **argv){
    signal(SIGINT,on_sigint);
    const char *dev=(argc>1&&argv[1][0]!='-')?argv[1]:NULL;
    const char *rpath=(argc>2)?argv[2]:"rosters/slave3.conf";
    if(argc>3) g_addr=(uint8_t)strtol(argv[3],NULL,16);

    roster_t roster; char err[128]={0};
    if(roster_load_file(rpath,&roster,err,sizeof err)!=0){ fprintf(stderr,"[piob] roster: %s\n",err); return 1; }
    usb_link_cfg_t lc={.device=dev,.baud=115200,.assert_dtr=1,.reconnect_ms_min=200,.reconnect_ms_max=1000};
    link_endpoint_t *ep=usb_link_create(&lc,NULL,NULL,NULL); if(!ep){fprintf(stderr,"usb_link_create failed\n");return 1;}
    controller_t *ctrl=controller_create(ep); if(!ctrl){fprintf(stderr,"controller_create failed\n");return 1;}
    controller_attach_roster(ctrl,&roster);

    printf("[piob] syncing...\n");
    int en=0; uint64_t dl=mono_ms()+8000;
    while(!en&&mono_ms()<dl&&!g_stop){ controller_poll(ctrl);
        if(controller_prov_state(ctrl)==PROV_DONE){controller_set_poll_enable(ctrl,1);en=1;}
        else if(controller_prov_state(ctrl)==PROV_FAIL){fprintf(stderr,"[piob] prov FAILED\n");return 1;} nap_ms(3);}
    if(!en){fprintf(stderr,"[piob] never OPERATIONAL\n");return 1;}
    printf("[piob] OPERATIONAL; SAMD21 @0x%02X\n\n", g_addr);

    /* 63 B (reg+data == HIL_I2C_MAX_LEN). Separate cfg per pin; out_ok mandatory
     * alongside out_err; watch[D1:1] is implicit-eq. pio_il uses only out_err. */
    const char *dsl = "g;cfg[D1:in];cfg[D2:out];watch[D1:1];out_ok[D2:1];out_err[D2:0]";
    printf("  load interlock_cfg DSL: \"%s\"\n", dsl);
    int sw = store_write(ctrl, REC_INTERLOCK_CFG, (const uint8_t*)dsl, (uint8_t)strlen(dsl));
    if(sw!=0){ printf("    store_write FAILED (%d)\n", sw); g_pass=0; }
    store_dump(ctrl, REC_INTERLOCK_CFG, (uint8_t)strlen(dsl));

    reg_write(ctrl, REG_MODE, MODE_PIO);          /* arm: parse record + set up INT */
    reg_write(ctrl, REG_IODIR, 0x00);             /* all channels outputs (D1/D2 master-driven) */
    reg_write(ctrl, REG_OLAT, 0xFF);              /* D1=1 -> watch OK; D2=1 */
    nap_ms(20);
    reg_write(ctrl, REG_INTFLAGS, 0x01);          /* manual reset: clear any boot/float trip */
    nap_ms(20);

    printf("\n  [armed, no fault]\n");
    expect("ILSTAT (0=parsed ok)", rd(ctrl,REG_ILSTAT), 0x00);
    expect("ILSTATE (valid+cond-ok)", rd(ctrl,REG_ILSTATE), 0x06);
    expect("INT_FLAGS", rd(ctrl,REG_INTFLAGS), 0x00);
    expect("GPIO (D2 master-high)", rd(ctrl,REG_GPIO), 0xFF);

    printf("\n  [trigger fault: drive D1=0 via OLAT]\n");
    reg_write(ctrl, REG_OLAT, 0xFD);              /* CH1(D1)=0 -> watch fails -> TRIP */
    nap_ms(20);
    expect("INT_FLAGS (tripped)", rd(ctrl,REG_INTFLAGS), 0x01);
    expect("GPIO (D2 forced 0)", rd(ctrl,REG_GPIO), 0xF9);   /* 0xFD & ~D2(bit2) */

    printf("\n  [manual reset with fault cleared]\n");
    reg_write(ctrl, REG_OLAT, 0xFF);              /* D1=1 again (fault gone) */
    reg_write(ctrl, REG_INTFLAGS, 0x01);          /* manual reset */
    nap_ms(20);
    expect("INT_FLAGS (recovered)", rd(ctrl,REG_INTFLAGS), 0x00);
    expect("GPIO (D2 back to OLAT)", rd(ctrl,REG_GPIO), 0xFF);

    printf("\n  [manual reset while fault persists -> re-trips]\n");
    reg_write(ctrl, REG_OLAT, 0xFD);              /* D1=0 -> trip */
    nap_ms(20);
    reg_write(ctrl, REG_INTFLAGS, 0x01);          /* reset, but fault still present */
    nap_ms(20);
    expect("INT_FLAGS (re-tripped)", rd(ctrl,REG_INTFLAGS), 0x01);

    /* cleanup */
    reg_write(ctrl, REG_OLAT, 0xFF);
    reg_write(ctrl, REG_INTFLAGS, 0x01);

    printf("\n[mon_pio_b] %s\n", g_pass?"PASS":"FAIL");
    controller_destroy(ctrl); link_close(ep);
    return g_pass?0:1;
}
