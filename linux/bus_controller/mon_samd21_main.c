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

/* config store (window regs 0x40 SEL / 0x41 LEN / 0x43 DATA / 0x44 CTRL / 0x45 STAT) */
#define REG_REC_SEL 0x40
#define REG_REC_LEN 0x41
#define REG_REC_DATA 0x43
#define REG_REC_CTRL 0x44
#define REG_STORE_STAT 0x45
static int store_write(controller_t *c, uint8_t rec, const uint8_t *data, uint8_t len) {
    if (reg_write(c, REG_REC_SEL, rec) != 0) return -1;        /* select record, off->0 */
    if (reg_write(c, REG_REC_LEN, len) != 0) return -1;        /* set length */
    uint8_t a[2+64]; a[0]=g_addr; a[1]=REG_REC_DATA;           /* burst-write the data port */
    if (len>64) len=64; memcpy(&a[2], data, len);
    reply_t r; if (call_to(c, APPCORE, CMD_I2C_WRITE, a, (uint16_t)(2+len), &r)!=0 || r.status!=0) return -1;
    if (reg_write(c, REG_REC_CTRL, 0xC0) != 0) return -1;      /* commit */
    for (int i=0;i<200;i++){ uint8_t st; if (reg_read(c,REG_STORE_STAT,&st,1)==0 && !(st&0x01)) return (st&0x02)?-2:0; nap_ms(5); }
    return -3;
}
static int store_read(controller_t *c, uint8_t rec, uint8_t *dst, uint8_t len) {
    if (reg_write(c, REG_REC_SEL, rec) != 0) return -1;        /* select record, off->0 */
    uint8_t a[3] = { g_addr, len, REG_REC_DATA };             /* write reg 0x43, read len (data port) */
    reply_t r; if (call_to(c, APPCORE, CMD_I2C_WRITE_READ, a, 3, &r)!=0 || r.status!=0 || r.len<len) return -1;
    memcpy(dst, r.buf, len); return 0;
}

/* commissioning: SET_ADDR (0x0F) is a data-port -> write [0x0F][0xAC magic][new_addr] */
static int set_addr(controller_t *c, uint8_t newaddr) {
    uint8_t a[4] = { g_addr, 0x0F, 0xAC, newaddr };
    reply_t r; return (call_to(c, APPCORE, CMD_I2C_WRITE, a, 4, &r)!=0 || r.status!=0) ? -1 : 0;
}
static void do_reset(controller_t *c) { reg_write(c, 0x0E, 0xA5); }  /* deferred soft-reset (may NACK) */
/* re-read WHO_AM_I at g_addr, retrying while the chip reboots */
static int whoami_retry(controller_t *c, uint8_t *out) {
    for (int i=0;i<12 && !g_stop;i++){ if (reg_read(c, REG_WHOAMI, out, 1)==0) return 0; nap_ms(400); }
    return -1;
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

    /* M2b: config store round-trip + flash persistence across a reset */
    printf("\n  --- config store (M2b) ---\n");
    const uint8_t pat[8] = { 0xDE,0xAD,0xBE,0xEF,0x01,0x02,0x03,0x04 };
    uint8_t rb[8];
    int sw = store_write(ctrl, 5 /*calibration*/, pat, 8);
    printf("  write+commit calibration: %s\n", sw==0?"ok":(sw==-2?"FAIL(store-err)":"FAIL"));
    if (sw!=0) pass=0;
    if (store_read(ctrl, 5, rb, 8)==0) { int ok=!memcmp(rb,pat,8);
        printf("  read back (RAM)  : %02X%02X%02X%02X%02X%02X%02X%02X %s\n", rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7], ok?"ok":"FAIL"); if(!ok)pass=0; }
    else { printf("  read back FAIL\n"); pass=0; }

    printf("  resetting SAMD21 (RESET reg) for persistence check...\n");
    reg_write(ctrl, 0x0E, 0xA5);     /* deferred soft-reset; may NACK as it drops */
    nap_ms(2500);                    /* wait for reboot */
    int got=0; for(int i=0;i<12 && !got && !g_stop;i++){ if(store_read(ctrl,5,rb,8)==0) got=1; else nap_ms(400); }
    if (got) { int ok=!memcmp(rb,pat,8);
        printf("  read back (FLASH): %02X%02X%02X%02X%02X%02X%02X%02X %s\n", rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7], ok?"ok PERSISTED":"FAIL"); if(!ok)pass=0; }
    else { printf("  read back after reset FAIL (no response)\n"); pass=0; }

    /* M2c: Modbus-style address commissioning (0x55 -> 0x42 -> restore 0x55) */
    printf("\n  --- address commissioning (M2c) ---\n");
    uint8_t tgt = 0x42, w = 0;
    printf("  SET_ADDR 0x%02X -> 0x%02X, reset...\n", g_addr, tgt);
    if (set_addr(ctrl, tgt) != 0) { printf("  SET_ADDR FAIL\n"); pass=0; }
    nap_ms(60); do_reset(ctrl); nap_ms(2500);
    g_addr = tgt;
    if (whoami_retry(ctrl, &w)==0 && w==0x5A) printf("  WHO_AM_I @0x%02X = 0x%02X  ok COMMISSIONED\n", tgt, w);
    else { printf("  WHO_AM_I @0x%02X = 0x%02X  FAIL\n", tgt, w); pass=0; }

    printf("  SET_ADDR 0x%02X -> 0x55 (restore), reset...\n", tgt);
    if (set_addr(ctrl, 0x55) != 0) { printf("  restore SET_ADDR FAIL\n"); pass=0; }
    nap_ms(60); do_reset(ctrl); nap_ms(2500);
    g_addr = 0x55;
    if (whoami_retry(ctrl, &w)==0 && w==0x5A) printf("  WHO_AM_I @0x55 = 0x%02X  ok RESTORED\n", w);
    else { printf("  WHO_AM_I @0x55 = 0x%02X  FAIL\n", w); pass=0; }

    printf("\n[mon_samd21] %s\n", pass?"PASS":"FAIL");
    controller_destroy(ctrl); link_close(ep);
    return pass?0:1;
}
