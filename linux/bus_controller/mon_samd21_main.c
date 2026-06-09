/* mon_samd21 — exercise the SAMD21 I2C client's M2a control bank (via the Pico
 * I2C master). Register-mapped: write [reg] then read (write-read), or write
 * [reg][val] to set a register.
 *
 *   reads WHO_AM_I/VERSION/UNIQUE_ID, reads MODE, writes MODE, reads it back.
 *
 *   ./mon_samd21 <BC> [roster] [addr_hex]
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
/* control bank */
#define REG_WHOAMI   0x00
#define REG_VERSION  0x01
#define REG_MODE     0x02
#define REG_STATUS   0x03
#define REG_I2CADDR  0x05
#define REG_UNIQUEID 0x06
#define MODE_ADC     2

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void nap_ms(unsigned ms) { struct timespec t = { ms/1000, (long)(ms%1000)*1000000 }; nanosleep(&t,NULL); }

typedef struct { int done; uint8_t status; uint8_t buf[256]; uint16_t len; } reply_t;
static reply_t g_reply;
static void on_reply(void *u, uint16_t rid, uint8_t st, const uint8_t *r, uint16_t len) {
    (void)rid; reply_t *s=(reply_t*)u; s->status=st; s->len=(len>sizeof s->buf)?(uint16_t)sizeof s->buf:len;
    if (r && s->len) memcpy(s->buf,r,s->len); s->done=1;
}
static int call_to(controller_t *c, uint8_t addr, uint16_t cmd, const uint8_t *args, uint16_t alen, reply_t *out) {
    g_reply.done=0; uint16_t rid=controller_send_shell_to(c,addr,cmd,args,alen,on_reply,&g_reply);
    if (rid==0xFFFF) return -1; uint64_t dl=mono_ms()+4000;
    while (!g_reply.done && mono_ms()<dl && !g_stop) { controller_poll(c); nap_ms(2); }
    *out=g_reply; return g_reply.done?0:-1;
}

static uint8_t g_addr = 0x55;
static int reg_read(controller_t *c, uint8_t reg, uint8_t *dst, uint8_t n) {
    uint8_t a[3] = { g_addr, n, reg };       /* [addr][rlen][reg] -> write reg, read n */
    reply_t r; if (call_to(c, APPCORE, CMD_I2C_WRITE_READ, a, 3, &r) != 0 || r.status != 0 || r.len < n) return -1;
    memcpy(dst, r.buf, n); return 0;
}
static int reg_write(controller_t *c, uint8_t reg, uint8_t val) {
    uint8_t a[3] = { g_addr, reg, val };     /* [addr][reg][val] */
    reply_t r; return (call_to(c, APPCORE, CMD_I2C_WRITE, a, 3, &r) != 0 || r.status != 0) ? -1 : 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    const char *dev   = (argc>1 && argv[1][0]!='-') ? argv[1] : NULL;
    const char *rpath = (argc>2) ? argv[2] : "rosters/slave3.conf";
    if (argc>3) g_addr = (uint8_t)strtol(argv[3], NULL, 16);

    roster_t roster; char err[128]={0};
    if (roster_load_file(rpath,&roster,err,sizeof err)!=0){ fprintf(stderr,"[samd21] roster: %s\n",err); return 1; }
    usb_link_cfg_t lc={ .device=dev,.baud=115200,.assert_dtr=1,.reconnect_ms_min=200,.reconnect_ms_max=1000 };
    link_endpoint_t *ep=usb_link_create(&lc,NULL,NULL,NULL); if(!ep){fprintf(stderr,"usb_link_create failed\n");return 1;}
    controller_t *ctrl=controller_create(ep); if(!ctrl){fprintf(stderr,"controller_create failed\n");return 1;}
    controller_attach_roster(ctrl,&roster);

    printf("[samd21] syncing...\n");
    int en=0; uint64_t dl=mono_ms()+8000;
    while(!en && mono_ms()<dl && !g_stop){ controller_poll(ctrl);
        if(controller_prov_state(ctrl)==PROV_DONE){controller_set_poll_enable(ctrl,1);en=1;}
        else if(controller_prov_state(ctrl)==PROV_FAIL){fprintf(stderr,"[samd21] prov FAILED\n");return 1;} nap_ms(3); }
    if(!en){fprintf(stderr,"[samd21] never OPERATIONAL\n");return 1;}
    printf("[samd21] OPERATIONAL; SAMD21 client @0x%02X (M2a control bank)\n\n", g_addr);

    int pass=1; uint8_t v, uid[8], mode;

    if (reg_read(ctrl, REG_WHOAMI, &v, 1)==0) { int ok=(v==0x5A); printf("  WHO_AM_I = 0x%02X %s\n", v, ok?"ok":"FAIL(exp 0x5A)"); if(!ok)pass=0; }
    else { printf("  WHO_AM_I read FAIL\n"); pass=0; }

    if (reg_read(ctrl, REG_VERSION, &v, 1)==0) printf("  VERSION  = 0x%02X\n", v); else { printf("  VERSION read FAIL\n"); pass=0; }

    if (reg_read(ctrl, REG_UNIQUEID, uid, 8)==0) {
        printf("  UNIQUE_ID= "); for(int i=0;i<8;i++) printf("%02X", uid[i]);
        int nz=0; for(int i=0;i<8;i++) if(uid[i]) nz=1; printf(" %s\n", nz?"ok":"FAIL(all-zero)"); if(!nz)pass=0;
    } else { printf("  UNIQUE_ID read FAIL\n"); pass=0; }

    if (reg_read(ctrl, REG_I2CADDR, &v, 1)==0) { int ok=(v==g_addr); printf("  I2C_ADDR = 0x%02X %s\n", v, ok?"ok":"FAIL"); if(!ok)pass=0; }
    else { printf("  I2C_ADDR read FAIL\n"); pass=0; }

    /* MODE round-trip: read (expect 0/idle), write ADC, read back */
    if (reg_read(ctrl, REG_MODE, &mode, 1)==0) printf("  MODE     = %u (start)\n", mode); else { printf("  MODE read FAIL\n"); pass=0; }
    if (reg_write(ctrl, REG_MODE, MODE_ADC)!=0) { printf("  MODE write FAIL\n"); pass=0; }
    nap_ms(20);
    if (reg_read(ctrl, REG_MODE, &mode, 1)==0) { int ok=(mode==MODE_ADC); printf("  MODE <- %u, read %u  %s\n", MODE_ADC, mode, ok?"ok":"FAIL"); if(!ok)pass=0; }
    else { printf("  MODE re-read FAIL\n"); pass=0; }

    printf("\n[mon_samd21] %s\n", pass?"PASS":"FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass?0:1;
}
