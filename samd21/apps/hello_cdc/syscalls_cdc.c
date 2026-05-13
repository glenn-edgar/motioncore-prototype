// newlib retargeting: forward stdout to TinyUSB CDC.
// Also catches malloc via a wrap so we notice if anything sneaks one in.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "tusb.h"

#undef errno
extern int errno;

// Write n bytes to fd. We only support stdout/stderr -> CDC.
__attribute__((used))
int _write(int fd, const char *buf, int n) {
  (void) fd;
  if (!tud_cdc_connected()) {
    // Pretend the write succeeded so printf doesn't block.
    return n;
  }
  int written = 0;
  while (written < n) {
    uint32_t w = tud_cdc_write(buf + written, (uint32_t)(n - written));
    if (w == 0) {
      tud_cdc_write_flush();
      tud_task();
      // Bail out if the host has gone away mid-write.
      if (!tud_cdc_connected()) break;
    }
    written += (int) w;
  }
  tud_cdc_write_flush();
  return written;
}

// Stubs that --specs=nosys.specs also provides, kept here so we don't have
// to pull nosys.specs in alongside nano.specs.  Defined as weak so nosys.specs
// versions win if both are linked.
__attribute__((weak)) int _read(int fd, char *buf, int n)        { (void)fd; (void)buf; (void)n; return 0; }
__attribute__((weak)) int _close(int fd)                          { (void)fd; return -1; }
__attribute__((weak)) int _lseek(int fd, int o, int w)            { (void)fd; (void)o; (void)w; return 0; }
__attribute__((weak)) int _fstat(int fd, void *st)                { (void)fd; (void)st; return 0; }
__attribute__((weak)) int _isatty(int fd)                         { (void)fd; return 1; }
__attribute__((weak)) int _getpid(void)                           { return 1; }
__attribute__((weak)) int _kill(int pid, int sig)                 { (void)pid; (void)sig; errno = 22; return -1; }
__attribute__((weak)) void _exit(int code)                        { (void)code; while (1) {} }

// Trap any heap allocations.  printf("%lu") on newlib-nano typically does NOT
// pull in malloc; floating point would.  We assert by aborting.
__attribute__((used))
void *__wrap_malloc(size_t sz) {
  (void) sz;
  // Park here so a debugger can catch it.
  for (;;) { __asm__ volatile("bkpt #0"); }
}
