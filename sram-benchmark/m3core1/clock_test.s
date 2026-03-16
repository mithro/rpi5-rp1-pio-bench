/* clock_test.s — M3 Core 1 clock speed measurement
 *
 * Two-phase test:
 *   Phase 1: Register-only loop (adds+b), 50M iterations
 *            Then write iteration count to status
 *   Phase 2: SRAM store loop (adds+str+b), continuous
 *
 * Host reads phase1_end_time - phase1_start_time (via counter)
 * to measure register-only loop rate, then compares with phase 2
 * (SRAM store) rate to isolate memory access overhead.
 *
 * Status layout at 0x20008D00:
 *   +0x00: magic        = 0xC0DE1234
 *   +0x04: counter      = incrementing heartbeat (phase 2)
 *   +0x08: phase        = 1 or 2
 *   +0x0C: phase1_iters = iterations completed in phase 1
 */

.cpu cortex-m3
.thumb
.syntax unified

.equ STATUS,  0x20008D00
.equ MAGIC,   0xC0DE1234
.equ PHASE1_COUNT, 1000000    /* 1M iterations */

.section .vectors, "a"
.align 2
.globl _vectors
_vectors:
    .word 0x200089F0
    .word _entry

.section .text
.align 2
.thumb_func
.globl _entry
_entry:
    ldr  r5, =STATUS

    /* Write magic and phase=1 */
    ldr  r0, =MAGIC
    str  r0, [r5, #0]
    mov  r0, #1
    str  r0, [r5, #8]       /* phase = 1 */
    mov  r0, #0
    str  r0, [r5, #12]      /* phase1_iters = 0 */

    /* Brief alive signal so host check_core1_alive sees counter change */
    mov  r0, #1
    str  r0, [r5, #4]
    mov  r0, #2
    str  r0, [r5, #4]
    mov  r0, #3
    str  r0, [r5, #4]

    /* Phase 1: register-only tight loop, 10M iterations.
     * No memory access in the loop body — measures pure
     * instruction execution speed (fetch from SRAM + execute). */
    ldr  r3, =PHASE1_COUNT
    mov  r2, #0
.phase1:
    adds r2, r2, #1
    cmp  r2, r3
    blt  .phase1

    /* Record phase 1 completion */
    str  r2, [r5, #12]      /* phase1_iters = 1M */

    /* Phase 2: SRAM store loop (same as sram_write_test) */
    mov  r0, #2
    str  r0, [r5, #8]       /* phase = 2 */
    mov  r2, #0
.phase2:
    adds r2, r2, #1
    str  r2, [r5, #4]       /* counter = r2 */
    b    .phase2
