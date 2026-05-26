/**
 * \file
 *
 * \brief Linker script for running in internal FLASH on the SAMD21G18A,
 * with a NOLOAD .noinit section added for the interlock framework's
 * persistence record (g_interlock_persist).
 *
 * Based on TinyUSB upstream
 *   hw/bsp/samd2x_l2x/boards/seeeduino_xiao/seeeduino_xiao.ld
 * which carries the original Microchip ASF copyright/SPDX header below.
 *
 * Changes vs upstream:
 *  1. `.noinit (NOLOAD)` output section, placed AFTER `.stack` so that
 *     it sits ABOVE the stack growth boundary. RAM in this section is
 *     NEITHER zero-initialised at startup NOR loaded from flash, so it
 *     survives warm resets (WDT bite / NVIC_SystemReset / EXT reset).
 *     POR / brown-out wipes it; the interlock framework detects this
 *     via a magic number and re-initialises.
 *  2. STACK_SIZE bumped from 0x800 (2 KB) to 0x1000 (4 KB). We had
 *     24 KB of unused RAM; slice 4's handle_shell_exec + s_engine_log
 *     stack frame depth pushed us to overflow at 2 KB. See
 *     samd21_stack_budget memory for analysis.
 *  3. Section order is now bss → stack → noinit. Stack overflow (SP
 *     dropping below _sstack) writes into .bss, not .noinit. Combined
 *     with the canary at _sstack and the heartbeat HWM check, overflow
 *     is detected and forces NVIC_SystemReset() before noinit is at
 *     risk.
 *
 * Copyright (c) 2017 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */


OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
SEARCH_DIR(.)

/* Memory Spaces Definitions */
MEMORY
{
  rom      (rx)  : ORIGIN = 0x00000000 + 8K, LENGTH = 0x00040000 - 8K  /* 8K offset to preserve bootloader */
  ram      (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00008000
}

/* The stack size used by the application. The Makefile passes
 * --defsym=__stack_size__=N to override. Default fallback is 0x2000 (8 KB).
 * Real value is set in the Makefile; see samd21_stack_budget memory.
 */
STACK_SIZE = DEFINED(__stack_size__) ? __stack_size__ : 0x2000;

ENTRY(Reset_Handler)

/* Section Definitions */
SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        _sfixed = .;
        KEEP(*(.vectors .vectors.*))
        *(.text .text.* .gnu.linkonce.t.*)
        *(.glue_7t) *(.glue_7)
        *(.rodata .rodata* .gnu.linkonce.r.*)
        *(.ARM.extab* .gnu.linkonce.armextab.*)

        /* Support C constructors, and C destructors in both user code
           and the C library. This also provides support for C++ code. */
        . = ALIGN(4);
        KEEP(*(.init))
        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP (*(.preinit_array))
        __preinit_array_end = .;

        . = ALIGN(4);
        __init_array_start = .;
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;

        . = ALIGN(4);
        KEEP (*crtbegin.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*crtend.o(.ctors))

        . = ALIGN(4);
        KEEP(*(.fini))

        . = ALIGN(4);
        __fini_array_start = .;
        KEEP (*(.fini_array))
        KEEP (*(SORT(.fini_array.*)))
        __fini_array_end = .;

        KEEP (*crtbegin.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*crtend.o(.dtors))

        . = ALIGN(4);
        _efixed = .;            /* End of text section */
    } > rom

    /* .ARM.exidx is sorted, so has to go in its own output section.  */
    PROVIDE_HIDDEN (__exidx_start = .);
    .ARM.exidx :
    {
      *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > rom
    PROVIDE_HIDDEN (__exidx_end = .);

    . = ALIGN(4);
    _etext = .;

    .relocate : AT (_etext)
    {
        . = ALIGN(4);
        _srelocate = .;
        *(.ramfunc .ramfunc.*);
        *(.data .data.*);
        . = ALIGN(4);
        _erelocate = .;
    } > ram

    /* .bss section which is used for uninitialized data */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = . ;
        _szero = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = . ;
        _ezero = .;
        end = .;
    } > ram

    /*
     * .stack — placed BEFORE .noinit (lower address than .noinit) so that
     * stack overflow (SP dropping below _sstack) writes into .bss instead
     * of corrupting noinit. The first 4 bytes at _sstack hold a known
     * canary written at boot; interlock_tick_all checks it every chain
     * pump tick and forces NVIC_SystemReset() if it changed.
     */
    .stack (NOLOAD):
    {
        . = ALIGN(8);
        _sstack = .;
        . = . + STACK_SIZE;
        . = ALIGN(8);
        _estack = .;
    } > ram

    /*
     * .noinit — interlock-framework persistence record. NOLOAD = no flash
     * image; startup does NOT copy/clear this region. Placed AFTER .stack
     * (above the stack region) so the stack growth boundary is on the
     * OTHER side of noinit — overflow goes into bss, not noinit. Survives
     * warm resets; wiped on POR/BOD (detected via magic mismatch in
     * interlock_boot_decide()).
     */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        _snoinit = .;
        KEEP(*(.noinit .noinit.*))
        . = ALIGN(4);
        _enoinit = .;
    } > ram

    . = ALIGN(4);
    _end = . ;
}
