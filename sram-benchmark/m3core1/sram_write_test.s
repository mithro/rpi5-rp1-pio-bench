/* sram_write_test.s — Minimal RP1 Core 1 firmware
 *
 * Proves Core 1 is running by writing a magic value and
 * continuously incrementing a counter in shared SRAM.
 *
 * Memory layout:
 *   Code:    0x20008B00 (vector table + code, in safe SRAM region)
 *   Stack:   0x200089F0 (grows down through 0x8000-0x89FF)
 *   Status:  0x20008D00 (magic + counter, readable by host)
 *
 * Status words at 0x20008D00:
 *   +0x00: magic     = 0xC0DE1234 (proves Core 1 ran)
 *   +0x04: counter   = incrementing (proves Core 1 is alive)
 *   +0x08: timestamp = SysTick value (if available)
 */

.cpu cortex-m3
.thumb
.syntax unified

/* ---- Vector table (at load address 0x20008B00) ---- */
.section .vectors, "a"
.align 2
.globl _vectors
_vectors:
    .word 0x200089F0    /* Initial SP (safe region, grows down) */
    .word _entry        /* Reset vector (entry point) */

/* ---- Code ---- */
.section .text
.align 2
.thumb_func
.globl _entry
_entry:
    /* Write magic value to status area */
    ldr r0, =0x20008D00     /* Status base address */
    ldr r1, =0xC0DE1234     /* Magic value */
    str r1, [r0]             /* status[0] = magic */

    /* Clear counter */
    mov r2, #0

    /* Main loop: increment counter forever */
.loop:
    adds r2, r2, #1
    str r2, [r0, #4]         /* status[1] = counter */
    b .loop
