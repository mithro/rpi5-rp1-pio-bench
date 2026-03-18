/* pio_fifo_test.s — Core 1 PIO FIFO diagnostic test
 *
 * One-shot diagnostic: writes 1 word to TXF3, force-executes PIO
 * instructions via SM3_INSTR, captures FSTAT/SM3_ADDR after each step
 * to trace exactly where data flow breaks down.
 *
 * Status layout at 0x20008D00:
 *   +0x00: magic        = 0xC0DE1234
 *   +0x04: counter      = incrementing (proves alive)
 *   +0x08: result       = 0:running, 1:PASS, 2:FAIL
 *   +0x0C: step         = last completed step (1-8)
 *   +0x10: tx_val       = value written to TXF3
 *   +0x14: rx_val       = value read from RXF3
 *   +0x18: exp_val      = expected value (~tx_val)
 *   +0x1C: fstat_init   = FSTAT after SM3 restart (all empty)
 *   +0x20: fstat_post_tx = FSTAT after writing to TXF3
 *   +0x24: addr_pre_pull = SM3_ADDR before pull
 *   +0x28: fstat_post_pull = FSTAT after forced pull block
 *   +0x2C: addr_post_pull  = SM3_ADDR after pull
 *   +0x30: fstat_post_push = FSTAT after forced push block
 *   +0x34: addr_post_push  = SM3_ADDR after push
 *   +0x38: fstat_pre_rx    = FSTAT before RX read
 */

.cpu cortex-m3
.thumb
.syntax unified

/* ── PIO register offsets (from M3 base 0xF0000000) ── */
.equ PIO_BASE,       0xF0000000
.equ PIO_CTRL,       0x00
.equ PIO_FSTAT,      0x04
.equ PIO_TXF3,       0x20      /* TXF0=0x14, +3*4 */
.equ PIO_RXF3,       0x30      /* RXF0=0x24, +3*4 */
.equ SM3_CLKDIV,     0x12C
.equ SM3_EXECCTRL,   0x130
.equ SM3_SHIFTCTRL,  0x134
.equ SM3_ADDR,       0x138
.equ SM3_INSTR,      0x13C
.equ SM3_PINCTRL,    0x140

.equ SM3_EN,         (1 << 3)
.equ TXFULL3,        (1 << 3)
.equ TXEMPTY3,       (1 << 11)
.equ RXFULL3,        (1 << 19)
.equ RXEMPTY3,       (1 << 27)

/* PIO instruction encodings */
.equ PIO_PULL_BLOCK,       0x80A0
.equ PIO_MOV_ISR_NOT_OSR,  0xA0CF
.equ PIO_PUSH_BLOCK,       0x8020

.equ STATUS,         0x20008D00
.equ MAGIC,          0xC0DE1234
.equ TEST_PATTERN,   0xDEADBEEF

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
    ldr  r4, =PIO_BASE
    ldr  r5, =STATUS

    /* Write magic and clear all status */
    ldr  r0, =MAGIC
    str  r0, [r5, #0x00]
    mov  r0, #0
    mov  r1, #0x3C           /* clear 15 words (0x04-0x3C) */
.clear:
    str  r0, [r5, r1]
    subs r1, r1, #4
    bne  .clear
    str  r0, [r5, #0x04]     /* also clear +0x04 */

    /* ── Step 1: Configure SM3 ── */
    mov  r0, #1
    str  r0, [r5, #0x0C]     /* step=1 */

    /* Disable SM3 */
    ldr  r0, [r4, #PIO_CTRL]
    bic  r0, r0, #SM3_EN
    str  r0, [r4, #PIO_CTRL]

    /* CLKDIV: integer=1, frac=0 */
    movw r0, #0
    movt r0, #1
    str.w r0, [r4, #SM3_CLKDIV]

    /* EXECCTRL: defaults */
    mov  r0, #0
    str.w r0, [r4, #SM3_EXECCTRL]

    /* SHIFTCTRL: defaults */
    mov  r0, #0
    str.w r0, [r4, #SM3_SHIFTCTRL]

    /* PINCTRL: no pins */
    mov  r0, #0
    str.w r0, [r4, #SM3_PINCTRL]

    /* SM_RESTART + CLKDIV_RESTART + SM_EN */
    ldr  r0, [r4, #PIO_CTRL]
    orr  r0, r0, #(1 << 3)
    orr  r0, r0, #(1 << 7)
    orr  r0, r0, #(1 << 11)
    str  r0, [r4, #PIO_CTRL]

    /* Delay */
    mov  r0, #200
1:  subs r0, r0, #1
    bne  1b

    /* ── Step 2: Capture initial state ── */
    mov  r0, #2
    str  r0, [r5, #0x0C]
    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x1C]     /* fstat_init */

    /* ── Step 3: Write test word to TXF3 ── */
    mov  r0, #3
    str  r0, [r5, #0x0C]
    ldr  r0, =TEST_PATTERN
    str  r0, [r4, #PIO_TXF3]
    str  r0, [r5, #0x10]     /* tx_val */

    /* DSB to ensure TX write completes before reading FSTAT */
    dsb

    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x20]     /* fstat_post_tx */

    /* ── Step 4: Force pull block ── */
    mov  r0, #4
    str  r0, [r5, #0x0C]
    ldr.w r0, [r4, #SM3_ADDR]
    str  r0, [r5, #0x24]     /* addr_pre_pull */

    movw r0, #PIO_PULL_BLOCK
    str.w r0, [r4, #SM3_INSTR]
    dsb
    /* Wait for pull to complete */
    mov  r0, #50
2:  subs r0, r0, #1
    bne  2b

    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x28]     /* fstat_post_pull */
    ldr.w r0, [r4, #SM3_ADDR]
    str  r0, [r5, #0x2C]     /* addr_post_pull */

    /* ── Step 5: Force mov isr, ~osr ── */
    mov  r0, #5
    str  r0, [r5, #0x0C]
    movw r0, #PIO_MOV_ISR_NOT_OSR
    str.w r0, [r4, #SM3_INSTR]
    dsb
    mov  r0, #50
3:  subs r0, r0, #1
    bne  3b

    /* ── Step 6: Force push block ── */
    mov  r0, #6
    str  r0, [r5, #0x0C]
    movw r0, #PIO_PUSH_BLOCK
    str.w r0, [r4, #SM3_INSTR]
    dsb
    mov  r0, #50
4:  subs r0, r0, #1
    bne  4b

    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x30]     /* fstat_post_push */
    ldr.w r0, [r4, #SM3_ADDR]
    str  r0, [r5, #0x34]     /* addr_post_push */

    /* ── Step 7: Read from RXF3 ── */
    mov  r0, #7
    str  r0, [r5, #0x0C]
    ldr  r0, [r4, #PIO_FSTAT]
    str  r0, [r5, #0x38]     /* fstat_pre_rx */

    /* Read RXF3 regardless of RXEMPTY (capture whatever is there) */
    ldr  r3, [r4, #PIO_RXF3]
    str  r3, [r5, #0x14]     /* rx_val */

    /* Expected = ~TEST_PATTERN */
    ldr  r0, =TEST_PATTERN
    mvn  r0, r0
    str  r0, [r5, #0x18]     /* exp_val */

    /* ── Step 8: Determine result ── */
    mov  r0, #8
    str  r0, [r5, #0x0C]

    /* Check data match only — FSTAT RXEMPTY bit may be unreliable
     * (RP1 may have TXEMPTY/RXEMPTY bits swapped vs RP2040) */
    ldr  r0, [r5, #0x14]     /* rx_val */
    ldr  r1, [r5, #0x18]     /* exp_val */
    cmp  r0, r1
    bne  .fail

    /* PASS */
    mov  r0, #1
    str  r0, [r5, #0x08]
    b    .spin

.fail:
    mov  r0, #2
    str  r0, [r5, #0x08]

.spin:
    /* Keep alive — increment counter forever */
    mov  r6, #0
.spin_loop:
    adds r6, r6, #1
    str  r6, [r5, #0x04]
    b    .spin_loop
