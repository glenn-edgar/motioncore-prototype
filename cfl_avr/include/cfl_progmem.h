/*
 * cfl_progmem.h — Harvard/von-Neumann portability shims.
 *
 * On AVR (Harvard), constant descriptor tables live in PROGMEM and require
 * LPM instructions to read. On Linux x86_64 (von Neumann), PROGMEM is a
 * no-op and pgm_read_* becomes a normal pointer dereference.
 *
 * Hot-path discipline: read each PROGMEM field exactly once per visit,
 * cache in a local. Do not let LPM nest inside LPM.
 */
#ifndef CFL_PROGMEM_H
#define CFL_PROGMEM_H

#include <stdint.h>
#include <string.h>

#ifdef __AVR__
  #include <avr/pgmspace.h>
  #include <avr/interrupt.h>

  /* Main-context atomic block:
   *   uint8_t s = cfl_atomic_save(); ... cfl_atomic_restore(s);
   * On AVR this saves SREG, disables interrupts, then restores SREG. */
  static inline uint8_t cfl_atomic_save(void) {
      uint8_t s = SREG;
      cli();
      return s;
  }
  static inline void cfl_atomic_restore(uint8_t s) {
      SREG = s;
  }
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #ifndef pgm_read_byte
    #define pgm_read_byte(addr)   (*(const uint8_t  *)(addr))
  #endif
  #ifndef pgm_read_word
    #define pgm_read_word(addr)   (*(const uint16_t *)(addr))
  #endif
  #ifndef memcpy_P
    #define memcpy_P(dst, src, n) memcpy((dst), (src), (n))
  #endif
  /* On non-AVR (Linux test harness) the atomic shim is a no-op. */
  static inline uint8_t cfl_atomic_save(void) { return 0; }
  static inline void    cfl_atomic_restore(uint8_t s) { (void)s; }
#endif

#endif /* CFL_PROGMEM_H */
