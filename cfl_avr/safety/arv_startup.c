/*
 * avr32sd_3v3_bringup.c
 * ---------------------------------------------------------------------------
 * AVR32SD32 (AVR(R) SD family) first-boot bring-up for the EV75S16A Curiosity
 * Nano, intended to run on a 3.3 V target rail.
 *
 * Toolchain : avr-gcc + avr-libc, with the Microchip AVR-Sx_DFP atpack
 *             supplying the device headers/startup (avr32sd32 is also a
 *             mainline avr-gcc -mmcu target in recent toolchains).
 *
 * What it does, all grounded in the datasheet you supplied (DS40002629):
 *   1. Runs CLK_MAIN at 20 MHz from the internal OSCHF.
 *        - Table 46-9 "System Clock Timing": fCLK_MAIN max = 20 MHz, the only
 *          constraint being the Standard Operating Conditions (Table 46-2):
 *          VDD 2.7-5.5 V, FLAT, no frequency-vs-VDD derating sub-table.
 *        - => full 20 MHz is in spec at 3.3 V. (Whole datasheet is even
 *          characterized at VDD = 3.0 V with OSCHF = 20 MHz as the nominal pt.)
 *   2. Configures the supply Voltage Level Monitor (VLM) inside the BOD as an
 *        early under-voltage warning, sized for a 3.3 V rail.
 *   3. Blinks LED0 as a heartbeat so you can confirm the silicon is alive; if
 *        the VLM trips, LED0 latches solid ON.
 *
 * This is step 1 of the bridge. ECC enable + the ERRCTRL-to-NMI handler are
 * the next layers, not in this file.
 * ---------------------------------------------------------------------------
 * BUILD
 *   PACK=/path/to/Microchip.AVR-Sx_DFP.<ver>
 *   avr-gcc -mmcu=avr32sd32 -O2 -DF_CPU=20000000UL \
 *       -B "$PACK/gcc/dev/avr32sd32" -I "$PACK/include" \
 *       avr32sd_3v3_bringup.c -o bringup.elf
 *   avr-objcopy -O ihex bringup.elf bringup.hex
 *
 * FUSE (set once, at programming time) -- the brown-out RESET floor.
 *   The BOD reset LEVEL and ACTIVE mode live in FUSE.BODCFG and CANNOT be
 *   raised at runtime (CTRLA.ACTIVE is loaded from the fuse and is locked).
 *   For a 3.3 V rail, set the reset floor to BODLEVEL2 = 2.70 V and enable BOD:
 *       BODCFG.LEVEL  = BODLEVEL2 (2.70 V)   <- datasheet 27.x VLMLVL table
 *       BODCFG.ACTIVE = enabled (continuous, or SAMPLED to save power)
 *   The runtime-adjustable part is the VLM warning configured below.
 *
 * FLASH over the Curiosity Nano on-board nEDBG (no MPLAB required)
 *   pymcuprog write -d avr32sd32 -t uart -u <serial-port> \
 *       -f bringup.hex --erase --verify
 *   # alt: avrdude -c jtag3updi -p avr32sd32 -U flash:w:bringup.hex:i
 *   Requires a pymcuprog / avrdude build that knows avr32sd32 (very new part;
 *   update the tool or point it at the atpack device file if missing).
 *
 *   TARGET VOLTAGE: the board's adjustable target regulator is commanded by the
 *   on-board debugger. Set Vtarget = 3.3 V (MPLAB project property, or your
 *   pymcuprog/pyedbglib build's voltage option) and CONFIRM it before trusting
 *   the VLM math below -- the warning level is computed relative to 3.3 V.
 * ---------------------------------------------------------------------------
 * A note on enum (_gc) macro spellings:
 *   The register and field names used here -- OSCHFCTRLA/FRQSEL, MCLKCTRLB/PEN,
 *   MCLKSTATUS/OSCHFS, BOD.VLMCTRLA/VLMLVL, BOD.INTCTRL/VLMCFG/VLMIE,
 *   BOD.INTFLAGS/VLMIF, BOD_VLM_vect -- are taken from DS40002629 and are
 *   correct in semantics. The exact group-code macro *spellings* (..._gc) come
 *   from the atpack's iom_avr32sd32.h. If any *_gc name below does not match
 *   your header, grep the header for the field (e.g. FRQSEL, VLMLVL) and swap
 *   in the right token; the bit semantics do not change.
 * ---------------------------------------------------------------------------
 */

#define F_CPU 20000000UL   /* CLK_PER after clock_20mhz(): OSCHF 20 MHz, no prescale */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

/* ===========================================================================
 *  Board pins (EV75S16A) from the Curiosity Nano schematic SCH 02-01254:
 *    LED0 = PD2, wired active-LOW (current sink): drive LOW = ON.
 *    SW0  = PF2, active-low, needs internal pull-up (unused in this file).
 *  Consistent with UG DS50003842 Table 3-2 (PF5 = heartbeat, not LED0).
 *  If GPIO misbehaves, re-confirm against UG Figure 1-3.
 * =========================================================================== */
#define LED0_VPORT   VPORTD     /* LED0 on PORT D ...        */
#define LED0_BIT     2          /* ... pin PD2 (active-LOW)  */

/* --------------------------------------------------------------------------- */

static volatile uint8_t supply_fault = 0;

/* Switch CLK_MAIN to 20 MHz from OSCHF. CLKCTRL regs are CCP-protected, so
 * every write goes through avr-libc's _PROTECTED_WRITE (emits the CCP key). */
static void clock_20mhz(void)
{
    /* After any reset, CLK_MAIN = OSCHF @ 4 MHz (datasheet 18.3.2).
     * Select 20 MHz and keep the main prescaler disabled so CLK_PER = 20 MHz. */
    _PROTECTED_WRITE(CLKCTRL.OSCHFCTRLA, CLKCTRL_FRQSEL_20M_gc);
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0x00);          /* PEN = 0 -> divide by 1 */

    /* CLKSEL already selects OSCHF out of reset, so no source switch needed.
     * Wait for the oscillator to report stable before relying on timing. */
    while (!(CLKCTRL.MCLKSTATUS & CLKCTRL_OSCHFS_bm)) { /* spin */ }
}

/* Voltage Level Monitor: early under-voltage warning sitting ABOVE the
 * fuse-set BOD reset floor.
 *
 * With the BOD reset floor at 2.70 V (BODLEVEL2), VLMLVL selects how far above
 * it the warning fires (datasheet 27.x):
 *      5%  -> ~2.84 V   15% -> ~3.11 V   25% -> ~3.38 V
 * On a 3.3 V nominal rail, 15% (~3.11 V) is the right pick: it warns when VDD
 * sags toward the floor yet clears the nominal rail with margin. 25% (~3.38 V)
 * sits above 3.3 V and would trip continuously -- do not use it here. */
static void supply_vlm_init(void)
{
    BOD.VLMCTRLA = BOD_VLMLVL_15ABOVE_gc;     /* verify _gc spelling in iom hdr */
    BOD.INTCTRL  = BOD_VLMCFG_BELOW_gc        /* IRQ when VDD crosses DOWN thru it */
                 | BOD_VLMIE_bm;
    sei();
}

/* VDD has sagged below ~3.11 V. In a real node this is where you would notify
 * the Error Controller / drive outputs to a safe state. For bring-up we just
 * latch a flag so the heartbeat goes solid and you can see it happened. */
ISR(BOD_VLM_vect)
{
    BOD.INTFLAGS = BOD_VLMIF_bm;   /* clear the flag (write 1 to clear) */
    supply_fault = 1;
}

int main(void)
{
    clock_20mhz();

    /* LED0 as output, start OFF (active-low -> drive HIGH = off). */
    LED0_VPORT.DIR |= (uint8_t)(1u << LED0_BIT);
    LED0_VPORT.OUT |= (uint8_t)(1u << LED0_BIT);

    supply_vlm_init();

    for (;;) {
        if (supply_fault) {
            /* Latched under-voltage warning: hold LED0 solid ON. */
            LED0_VPORT.OUT &= (uint8_t)~(1u << LED0_BIT);
        } else {
            /* Heartbeat. Writing 1 to VPORTx.IN toggles the matching OUT bit
             * on modern AVR -- single-instruction toggle, no read-modify-write. */
            LED0_VPORT.IN = (uint8_t)(1u << LED0_BIT);
            _delay_ms(250);
        }
    }
}


