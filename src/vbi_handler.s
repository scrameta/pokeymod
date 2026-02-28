; vbi_handler.s - Deferred VBI and IRQ hooks for PokeyMAX MOD player
;
; KEY ISSUES FIXED vs previous version:
;
; 1. cc65 C functions use zero page temporaries $05-$0C (ptr1/ptr2/tmp1 etc.)
;    The deferred VBI must save/restore these, or C code called from the
;    VBI will corrupt the main program's zero page state.
;
; 2. The IRQ handler chains correctly via JMP (indirect) but needs the
;    vector stored in a proper zero-page-indirect-safe location.
;
; 3. Register saves: the Atari OS deferred VBI saves A/X/Y before calling
;    us, but we still save them ourselves since we call C (which clobbers
;    everything) and then jump to XITVBV which expects clean state.

        .export _vbi_install
        .export _vbi_remove
        .import _mod_vbi_tick
        .import _pokeymax_loop_handler

; OS equates
VVBLKD      = $0224     ; deferred VBI vector (lo/hi)
VIMIRQ      = $0216     ; IRQ vector (lo/hi)
SETVBV      = $E45C     ; OS: set VBI vector. A=type(6=deferred), X=lo, Y=hi
XITVBV      = $E462     ; OS: exit deferred VBI

SAM_IRQACT  = $D292     ; PokeyMAX sample end IRQ active/clear

; cc65 zero page on Atari target starts at $82.
; It uses 26 bytes ($1A): sp, sreg, regsave, ptr1-4, tmp1-4, regbank
; i.e. $82..$9B. Save/restore all of these when calling C from ISR.
ZP_SAVE_START = $82
ZP_SAVE_LEN   = 26     ; save $82..$9B

;--------------------------------------------------------------
; Saved vectors
;--------------------------------------------------------------
.bss
old_vbi_lo:   .res 1
old_vbi_hi:   .res 1
old_irq_lo:   .res 1
old_irq_hi:   .res 1
zp_save:      .res ZP_SAVE_LEN   ; zero page save area

.code

;--------------------------------------------------------------
; vbi_install()
;--------------------------------------------------------------
.proc _vbi_install
        ; Save current deferred VBI vector
        lda VVBLKD
        sta old_vbi_lo
        lda VVBLKD+1
        sta old_vbi_hi

        ; Save current IRQ vector
        lda VIMIRQ
        sta old_irq_lo
        lda VIMIRQ+1
        sta old_irq_hi

        ; Install deferred VBI via OS SETVBV (A=6, X=lo, Y=hi)
        ldx #<our_vbi
        ldy #>our_vbi
        lda #6
        jsr SETVBV

        ; Install IRQ handler
        sei
        lda #<our_irq
        sta VIMIRQ
        lda #>our_irq
        sta VIMIRQ+1
        cli

        rts
.endproc

;--------------------------------------------------------------
; vbi_remove()
;--------------------------------------------------------------
.proc _vbi_remove
        ; Restore deferred VBI
        ldx old_vbi_lo
        ldy old_vbi_hi
        lda #6
        jsr SETVBV

        ; Restore IRQ
        sei
        lda old_irq_lo
        sta VIMIRQ
        lda old_irq_hi
        sta VIMIRQ+1
        cli

        rts
.endproc

;--------------------------------------------------------------
; our_vbi - deferred VBI routine
;
; The OS has already saved A/X/Y before calling us.
; We save/restore cc65 zero page temporaries so that
; calling C from the VBI doesn't corrupt the main program.
;--------------------------------------------------------------
.proc our_vbi
        ; Save registers
        pha
        txa
        pha
        tya
        pha

        ; Save cc65 zero page temporaries
        ldx #ZP_SAVE_LEN-1
@save:  lda ZP_SAVE_START,x
        sta zp_save,x
        dex
        bpl @save

        ; Call C tick function
        jsr _mod_vbi_tick

        ; Restore cc65 zero page temporaries
        ldx #ZP_SAVE_LEN-1
@rest:  lda zp_save,x
        sta ZP_SAVE_START,x
        dex
        bpl @rest

        ; Restore registers
        pla
        tay
        pla
        tax
        pla

        jmp XITVBV
.endproc

;--------------------------------------------------------------
; our_irq - IRQ handler
;
; Check if PokeyMAX sample-end IRQ fired. If so, call the
; loop handler. Then chain to original IRQ handler.
;
; We check IRQACT *first* before saving registers to be fast
; on the common case (not our IRQ).
;--------------------------------------------------------------
.proc our_irq
        ; Quick check: is this a PokeyMAX sample IRQ?
        pha                     ; save A first (needed for RTI path)
        lda SAM_IRQACT
        beq @chain              ; nothing from PokeyMAX, skip

        ; It's ours - save remaining regs and ZP
        txa
        pha
        tya
        pha

        ldx #ZP_SAVE_LEN-1
@save:  lda ZP_SAVE_START,x
        sta zp_save,x
        dex
        bpl @save

        jsr _pokeymax_loop_handler

        ; loop_handler clears IRQACT internally

        ldx #ZP_SAVE_LEN-1
@rest:  lda zp_save,x
        sta ZP_SAVE_START,x
        dex
        bpl @rest

        pla
        tay
        pla
        tax

@chain:
        pla                     ; restore A saved at entry
        jmp (old_irq_lo)        ; chain to previous handler
.endproc
