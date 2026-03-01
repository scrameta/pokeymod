; loop_handler_irq_compat.s - cc65-safe IRQ compatibility wrapper
;
; Exports _pokeymax_loop_irq_fast but implements it as an asm wrapper
; that preserves cc65 Atari runtime zero page ($82..$9B) before calling
; the C-compatible loop handler entry _pokeymax_loop_irq_c.

        .export _pokeymax_loop_irq_fast
        .import _pokeymax_loop_irq_c

ZP_SAVE_START = $82
ZP_SAVE_LEN   = 26

.bss
zp_save_irq_compat: .res ZP_SAVE_LEN

.code
.proc _pokeymax_loop_irq_fast
        ldx #ZP_SAVE_LEN-1
@save:  lda ZP_SAVE_START,x
        sta zp_save_irq_compat,x
        dex
        bpl @save

        jsr _pokeymax_loop_irq_c

        ldx #ZP_SAVE_LEN-1
@rest:  lda zp_save_irq_compat,x
        sta ZP_SAVE_START,x
        dex
        bpl @rest

        rts
.endproc
