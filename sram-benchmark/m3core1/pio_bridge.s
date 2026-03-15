/* pio_bridge.s — Core 1 SRAM↔PIO FIFO bridge firmware
 *
 * Continuously moves data: TX buffer → PIO TXF3 → RXF3 → RX buffer.
 * The host pre-configures SM3 with an autonomous PIO program
 * (pull → NOT → push) BEFORE launching Core 1. This firmware
 * does NOT touch any PIO configuration registers — it only
 * reads/writes the FIFO data registers.
 *
 * Data path per word:
 *   1. Load word from TX buffer in SRAM
 *   2. Write to TXF3 (SM3 auto-pulls from TX FIFO)
 *   3. DSB + small delay (wait for PIO to process)
 *   4. Read from RXF3 (SM3 auto-pushes to RX FIFO)
 *   5. Store to RX buffer in SRAM
 *
 * Memory layout:
 *   Status:    0x20008D00 (16 words)
 *   TX buffer: 0x20009000 (1024 words = 4 KB)
 *   RX buffer: 0x2000A000 (1024 words = 4 KB)
 *
 * Status layout at 0x20008D00:
 *   +0x00: magic        = 0xC0DE1234
 *   +0x04: counter      = incrementing heartbeat
 *   +0x08: result       = 0:running, 1:PASS, 2:FAIL
 *   +0x0C: words_done   = total words processed
 *   +0x10: error_count  = number of verify errors
 *   +0x14: first_err_idx = index of first error
 *   +0x18: first_err_got = actual value at first error
 *   +0x1C: first_err_exp = expected value at first error
 *   +0x20: passes_done  = number of complete passes through buffer
 *   +0x24: cmd          = host command (0=stop, 1=run, 2=run+verify)
 *
 * Host writes cmd=1 or cmd=2 to start processing.
 * Core 1 polls cmd; when 0, stops and reports result.
 */

.cpu cortex-m3
.thumb
.syntax unified

/* ── PIO FIFO registers only (from M3 base 0xF0000000) ── */
.equ PIO_BASE,       0xF0000000
.equ PIO_TXF3,       0x20
.equ PIO_RXF3,       0x30

/* Memory addresses */
.equ STATUS,         0x20008D00
.equ TX_BUF,         0x20009000
.equ RX_BUF,         0x2000A000
.equ BUF_WORDS,      1024
.equ MAGIC,          0xC0DE1234

/* Status field offsets */
.equ OFF_MAGIC,      0x00
.equ OFF_COUNTER,    0x04
.equ OFF_RESULT,     0x08
.equ OFF_WORDS_DONE, 0x0C
.equ OFF_ERR_COUNT,  0x10
.equ OFF_ERR_IDX,    0x14
.equ OFF_ERR_GOT,    0x18
.equ OFF_ERR_EXP,    0x1C
.equ OFF_PASSES,     0x20
.equ OFF_CMD,        0x24

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
    /* Load base pointers into high registers (preserved across loop) */
    ldr  r4, =PIO_BASE
    ldr  r5, =STATUS

    /* Write magic and clear status */
    ldr  r0, =MAGIC
    str  r0, [r5, #OFF_MAGIC]
    mov  r0, #0
    str  r0, [r5, #OFF_COUNTER]
    str  r0, [r5, #OFF_RESULT]
    str  r0, [r5, #OFF_WORDS_DONE]
    str  r0, [r5, #OFF_ERR_COUNT]
    str  r0, [r5, #OFF_ERR_IDX]
    str  r0, [r5, #OFF_ERR_GOT]
    str  r0, [r5, #OFF_ERR_EXP]
    str  r0, [r5, #OFF_PASSES]
    /* Don't clear CMD — host sets it before launch */

    /* No PIO configuration here — host has already set up SM3
     * with an autonomous pull→NOT→push program via BAR1. */

    /* r11 = heartbeat counter */
    mov  r11, #0

    /* ── Wait for host command ── */
.wait_cmd:
    adds r11, r11, #1
    str  r11, [r5, #OFF_COUNTER]
    ldr  r0, [r5, #OFF_CMD]
    cmp  r0, #0
    beq  .wait_cmd

    /* r7 = cmd (1=run no verify, 2=run+verify) */
    mov  r7, r0

    /* Initialize counters */
    mov  r0, #0
    mov  r6, r0               /* r6 = words_done (running total) */
    str  r0, [r5, #OFF_WORDS_DONE]

    /* ── Main processing loop ── */
.next_pass:
    ldr  r0, =TX_BUF
    mov  r12, r0              /* r12 = TX pointer */
    ldr  r0, =RX_BUF
    mov  lr, r0               /* lr = RX pointer (safe: not in subroutine) */
    mov  r3, #0               /* r3 = word index within pass */

.word_loop:
    /* Simple loop: write TXF3, read RXF3, no explicit delay needed.
     * The str to Device memory stalls CPU until write reaches PIO.
     * The ldr from Device memory also takes multiple bus cycles.
     * PIO needs 3 cycles (15ns) — well within bus latency. */
    ldr  r0, [r12, r3, lsl #2]   /* load TX[r3] from SRAM */
    str  r0, [r4, #PIO_TXF3]     /* write to PIO TX FIFO */
    ldr  r1, [r4, #PIO_RXF3]     /* read from PIO RX FIFO */
    str  r1, [lr, r3, lsl #2]    /* store RX[r3] to SRAM */
    adds r3, r3, #1
    cmp  r3, #BUF_WORDS
    blt  .word_loop

    /* ── Pass complete — update counters ── */
    movw r0, #(BUF_WORDS & 0xFFFF)
    adds r6, r6, r0
    str  r6, [r5, #OFF_WORDS_DONE]
    adds r11, r11, #1
    str  r11, [r5, #OFF_COUNTER]
    ldr  r0, [r5, #OFF_PASSES]
    adds r0, r0, #1
    str  r0, [r5, #OFF_PASSES]

    /* Check if host wants verify (cmd=2) */
    cmp  r7, #2
    bne  .no_verify

    /* ── Verify pass: compare RX buffer against ~TX buffer ── */
    mov  r3, #0
.verify_loop:
    ldr  r0, [r12, r3, lsl #2]   /* TX word */
    mvn  r0, r0                   /* expected = ~TX */
    ldr  r1, [lr, r3, lsl #2]    /* RX word */
    cmp  r0, r1
    bne  .verify_err
.verify_next:
    adds r3, r3, #1
    cmp  r3, #BUF_WORDS
    blt  .verify_loop
    b    .no_verify

.verify_err:
    /* Record first error only */
    ldr  r2, [r5, #OFF_ERR_COUNT]
    cmp  r2, #0
    bne  .verify_err_count
    str  r3, [r5, #OFF_ERR_IDX]
    str  r1, [r5, #OFF_ERR_GOT]
    str  r0, [r5, #OFF_ERR_EXP]
.verify_err_count:
    adds r2, r2, #1
    str  r2, [r5, #OFF_ERR_COUNT]
    b    .verify_next

.no_verify:
    /* Check if host wants to stop (cmd=0) */
    ldr  r0, [r5, #OFF_CMD]
    cmp  r0, #0
    beq  .done

    /* Continue to next pass */
    b    .next_pass

.done:
    /* Determine result: PASS if no errors, FAIL otherwise */
    ldr  r0, [r5, #OFF_ERR_COUNT]
    cmp  r0, #0
    beq  .pass
    mov  r0, #2              /* FAIL */
    str  r0, [r5, #OFF_RESULT]
    b    .spin
.pass:
    mov  r0, #1              /* PASS */
    str  r0, [r5, #OFF_RESULT]

.spin:
    adds r11, r11, #1
    str  r11, [r5, #OFF_COUNTER]
    b    .spin
