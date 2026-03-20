/* pio_addr_test.s — Compare PIO access latency at two M3 addresses
 *
 * Tests whether PIO registers are accessible at the standard
 * peripheral bus address (0x40178000) vs the known-working
 * fast-path alias (0xF0000000) from M3 Core 1.
 *
 * Phase 1: One-shot diagnostic reads from both addresses
 * Phase 2: Throughput loop reading FSTAT at 0xF0000000 (10M iter)
 * Phase 3: Throughput loop reading FSTAT at 0x40178000 (10M iter)
 * Phase 4: Idle heartbeat (done)
 *
 * Status layout at 0x20008D00:
 *   +0x00: magic         = 0xC0DE1234
 *   +0x04: counter       = heartbeat (increments in all phases)
 *   +0x08: result        = 0:running, 1:done
 *   +0x0C: step          = current phase (1-4)
 *   +0x10: fstat_f0      = FSTAT read from 0xF0000000+0x04
 *   +0x14: fstat_40      = FSTAT read from 0x40178000+0x04
 *   +0x18: phase2_iters  = iterations completed in phase 2
 *   +0x1C: phase3_iters  = iterations completed in phase 3
 *
 * If 0x40178000 is not accessible, Core 1 will HardFault during
 * phase 1 step 2 — host will see counter stop with step=1.
 */

.cpu cortex-m3
.thumb
.syntax unified

.equ PIO_F0_BASE,  0xF0000000    /* Fast-path PIO alias */
.equ PIO_40_BASE,  0x40178000    /* Standard peripheral bus PIO */
.equ PIO_FSTAT,    0x04

.equ STATUS,       0x20008D00
.equ MAGIC,        0xC0DE1234
.equ PHASE_COUNT,  10000000       /* 10M iterations per phase */

/* ── Vector table ── */
.section .vectors, "a"
.align 2
.globl _vectors
_vectors:
    .word 0x200089F0
    .word _entry

/* ── Code ── */
.section .text
.align 2
.thumb_func
.globl _entry
_entry:
    ldr  r5, =STATUS

    /* Write magic, clear all status fields */
    ldr  r0, =MAGIC
    str  r0, [r5, #0x00]
    mov  r0, #0
    str  r0, [r5, #0x04]      /* counter = 0 */
    str  r0, [r5, #0x08]      /* result = 0 (running) */
    str  r0, [r5, #0x0C]      /* step = 0 */
    str  r0, [r5, #0x10]      /* fstat_f0 = 0 */
    str  r0, [r5, #0x14]      /* fstat_40 = 0 */
    str  r0, [r5, #0x18]      /* phase2_iters = 0 */
    str  r0, [r5, #0x1C]      /* phase3_iters = 0 */

    /* Brief alive signal so host check_core1_alive sees change */
    mov  r0, #1
    str  r0, [r5, #0x04]
    mov  r0, #2
    str  r0, [r5, #0x04]
    mov  r0, #3
    str  r0, [r5, #0x04]

    /* ─── Phase 1: One-shot diagnostic reads ─── */
    mov  r0, #1
    str  r0, [r5, #0x0C]      /* step = 1 (phase 1) */

    /* Read FSTAT from 0xF0000000 base (known working) */
    ldr  r4, =PIO_F0_BASE
    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x10]      /* fstat_f0 */

    /* Read FSTAT from 0x40178000 base (may HardFault!) */
    ldr  r0, =0xDEADDEAD      /* sentinel value */
    str  r0, [r5, #0x14]      /* pre-fill fstat_40 with sentinel */

    ldr  r4, =PIO_40_BASE
    ldr  r0, [r4, #PIO_FSTAT] /* THIS MAY FAULT */
    str  r0, [r5, #0x14]      /* fstat_40 (overwrite sentinel) */

    /* ─── Phase 2: Throughput at 0xF0000000 (10M iterations) ─── */
    mov  r0, #2
    str  r0, [r5, #0x0C]      /* step = 2 (phase 2) */

    ldr  r4, =PIO_F0_BASE
    ldr  r3, =PHASE_COUNT
    mov  r2, #0
    mov  r7, #4               /* counter offset (avoid recalc) */
.phase2:
    ldr  r0, [r4, #PIO_FSTAT]
    adds r2, r2, #1
    str  r2, [r5, r7]         /* counter = r2 */
    cmp  r2, r3
    blt  .phase2

    str  r2, [r5, #0x18]      /* phase2_iters = 10M */

    /* ─── Phase 3: Throughput at 0x40178000 (10M iterations) ─── */
    mov  r0, #3
    str  r0, [r5, #0x0C]      /* step = 3 (phase 3) */

    ldr  r4, =PIO_40_BASE
    mov  r2, #0
.phase3:
    ldr  r0, [r4, #PIO_FSTAT]
    adds r2, r2, #1
    str  r2, [r5, r7]         /* counter = r2 */
    cmp  r2, r3
    blt  .phase3

    str  r2, [r5, #0x1C]      /* phase3_iters = 10M */

    /* ─── Phase 4: Done, heartbeat only ─── */
    mov  r0, #4
    str  r0, [r5, #0x0C]      /* step = 4 (done) */
    mov  r0, #1
    str  r0, [r5, #0x08]      /* result = 1 (PASS/done) */

    mov  r2, #0
.heartbeat:
    adds r2, r2, #1
    str  r2, [r5, r7]         /* counter */
    b    .heartbeat
