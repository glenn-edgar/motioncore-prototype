/* mon_adc_mode — exercise the SAMD21 ADC mode (ADC-a: free-running stats + DAC).
 *
 * Jumper A0->A1 (DAC out D0 -> ADC CH0 = D1/AIN4). Drives the DAC constant, reads
 * the CH0 tumbling-window stats: avg tracks the DAC (~4x), min~=max~=avg and rms~0
 * for a constant signal, and the window seq id advances ~10/s on the 10 Hz window.
 *
 *   ./mon_adc_mode <BC> [roster] [addr_hex]
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
#define MODE_ADC     2
#define REG_CH_SEL   0x10
#define REG_WIN_SEL  0x11
#define REG_BLOCK    0x12          /* seq,min,max,avg,rms = 10 B */
#define REG_DAC_MODE 0x20
#define REG_DAC_LVL  0x21          /* u16 */
#define REG_DAC_APPLY 0x25

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop=1; }
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
static int reg_write(controller_t *c, uint8_t reg, uint8_t val){
    uint8_t a[3]={g_addr,reg,val}; reply_t r; return (call_to(c,APPCORE,CMD_I2C_WRITE,a,3,&r)!=0||r.status!=0)?-1:0;
}
static int reg_read(controller_t *c, uint8_t reg, uint8_t *dst, uint8_t n){
    uint8_t a[3]={g_addr,n,reg}; reply_t r;
    if(call_to(c,APPCORE,CMD_I2C_WRITE_READ,a,3,&r)!=0||r.status!=0||r.len<n) return -1; memcpy(dst,r.buf,n); return 0;
}
/* read the 10-byte stats block for the current (ch,win) with a seqlock retry */
static int read_block(controller_t *c, uint16_t *seq, uint16_t *mn, uint16_t *mx, uint16_t *avg, uint16_t *rms){
    uint8_t b[10]; if(reg_read(c,REG_BLOCK,b,10)!=0) return -1;   /* seq+stats atomic on the wire */
    *seq=(uint16_t)(b[0]|(b[1]<<8)); *mn=(uint16_t)(b[2]|(b[3]<<8)); *mx=(uint16_t)(b[4]|(b[5]<<8));
    *avg=(uint16_t)(b[6]|(b[7]<<8)); *rms=(uint16_t)(b[8]|(b[9]<<8)); return 0;
}
static int dac_set(controller_t *c, uint16_t lvl){
    if(reg_write(c,REG_DAC_MODE,0)!=0) return -1;
    if(reg_write(c,REG_DAC_LVL,(uint8_t)lvl)!=0) return -1;
    if(reg_write(c,REG_DAC_LVL+1,(uint8_t)(lvl>>8))!=0) return -1;
    return reg_write(c,REG_DAC_APPLY,1);
}

int main(int argc, char **argv){
    signal(SIGINT,on_sigint);
    const char *dev=(argc>1&&argv[1][0]!='-')?argv[1]:NULL;
    const char *rpath=(argc>2)?argv[2]:"rosters/slave3.conf";
    if(argc>3) g_addr=(uint8_t)strtol(argv[3],NULL,16);

    roster_t roster; char err[128]={0};
    if(roster_load_file(rpath,&roster,err,sizeof err)!=0){ fprintf(stderr,"[adc] roster: %s\n",err); return 1; }
    usb_link_cfg_t lc={.device=dev,.baud=115200,.assert_dtr=1,.reconnect_ms_min=200,.reconnect_ms_max=1000};
    link_endpoint_t *ep=usb_link_create(&lc,NULL,NULL,NULL); if(!ep){fprintf(stderr,"usb_link_create failed\n");return 1;}
    controller_t *ctrl=controller_create(ep); if(!ctrl){fprintf(stderr,"controller_create failed\n");return 1;}
    controller_attach_roster(ctrl,&roster);

    printf("[adc] syncing...\n");
    int en=0; uint64_t dl=mono_ms()+8000;
    while(!en&&mono_ms()<dl&&!g_stop){ controller_poll(ctrl);
        if(controller_prov_state(ctrl)==PROV_DONE){controller_set_poll_enable(ctrl,1);en=1;}
        else if(controller_prov_state(ctrl)==PROV_FAIL){fprintf(stderr,"[adc] prov FAILED\n");return 1;} nap_ms(3);}
    if(!en){fprintf(stderr,"[adc] never OPERATIONAL\n");return 1;}
    printf("[adc] OPERATIONAL; SAMD21 @0x%02X\n\n", g_addr);

    int pass=1;
    reg_write(ctrl, REG_MODE, MODE_ADC);
    uint8_t m=0xFF; reg_read(ctrl,REG_MODE,&m,1);
    printf("  MODE = %u %s\n", m, m==MODE_ADC?"ok(ADC)":"FAIL"); if(m!=MODE_ADC) pass=0;
    reg_write(ctrl, REG_CH_SEL, 0);          /* CH0 = D1/AIN4 (jumpered to DAC) */
    reg_write(ctrl, REG_WIN_SEL, 0);         /* 10 Hz window */

    /* seq-rate check: 10 Hz window should advance ~10 counts/sec */
    uint16_t s0,mn,mx,av,rms; dac_set(ctrl, 512); nap_ms(300);
    read_block(ctrl,&s0,&mn,&mx,&av,&rms);
    nap_ms(1000);
    uint16_t s1; read_block(ctrl,&s1,&mn,&mx,&av,&rms);
    int rate=(int)(uint16_t)(s1-s0);
    printf("  10Hz window seq rate = %d /s  %s (expect ~8-12)\n", rate, (rate>=6&&rate<=14)?"ok":"FAIL");
    if(rate<6||rate>14) pass=0;

    /* DAC sweep: avg should track ~4x level; constant signal => min~max~avg, rms small */
    printf("\n  DAC sweep (CH0, 10Hz window) — avg should be ~4x level:\n");
    const uint16_t lv[]={128,256,512,768,1000};
    for(unsigned i=0;i<sizeof lv/sizeof lv[0] && !g_stop;i++){
        dac_set(ctrl, lv[i]); nap_ms(250);
        uint16_t seq; if(read_block(ctrl,&seq,&mn,&mx,&av,&rms)!=0){ printf("    block read FAIL\n"); pass=0; continue; }
        int exp=lv[i]*4; int derr=(int)av-exp; if(derr<0)derr=-derr;
        int span=(int)mx-(int)mn;
        int ok = (derr <= exp/20+40) && (span<=40) && (rms<=20);   /* ~5%+40 tol; constant => tight span, low rms */
        printf("    DAC=%4u  avg=%4u (exp~%4u d=%+d)  min=%4u max=%4u rms=%3u  %s\n",
               lv[i], av, exp, (int)av-exp, mn, mx, rms, ok?"ok":"CHECK");
        if(!ok) pass=0;
    }

    printf("\n[mon_adc_mode] %s\n", pass?"PASS":"FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass?0:1;
}
