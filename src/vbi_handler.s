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
        .import _pokeymax_loop_irq_fast
        .importzp c_sp

; OS equates
VVBLKD      = $0224     ; deferred VBI vector (lo/hi)
VIMIRQ      = $0216     ; IRQ vector (lo/hi)
SETVBV      = $E45C     ; OS: set VBI vector. A=type(6=deferred), X=lo, Y=hi
XITVBV      = $E462     ; OS: exit deferred VBI

SAM_IRQACT  = $D292     ; PokeyMAX sample end IRQ active/clear
COLBK       = $D01A     ; GTIA background color

BUSY_COLOR_VBI = $38    ; red green pulse while VBI music tick runs
BUSY_COLOR_IRQ = $68    ; blue pulse while sample IRQ loop handler runs

; cc65 zero page on Atari target starts at $82.
; It uses 26 bytes ($1A): c_sp, sreg, regsave, ptr1-4, tmp1-4, regbank
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
zp_save_vbi:  .res ZP_SAVE_LEN   ; VBI zero page save area
saved_sp:     .res 2

; Dedicated C stack while running the deferred VBI C tick.
; This prevents re-entrant use of the foreground's cc65 software stack,
; which otherwise causes memory corruption when NMI interrupts C code.
VBI_CSTACK_SIZE = 160
vbi_cstack:    .res VBI_CSTACK_SIZE

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

        ; Install deferred VBI via OS SETVBV (A=7, X=hi, Y=lo)
        ldy #<our_vbi          ; Y = low byte
        ldx #>our_vbi          ; X = high byte
        lda #7                 ; deferred VBI (VVBLKD)
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
        ldy old_vbi_lo         ; Y = low byte
        ldx old_vbi_hi         ; X = high byte
        lda #7                 ; deferred VBI
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
        ; Mark CPU busy in visible way: pulse background color while tick runs.
        lda #BUSY_COLOR_VBI
        sta COLBK

        ; Save registers
        pha
        txa
        pha
        tya
        pha

        ; Save cc65 zero page temporaries
        ldx #ZP_SAVE_LEN-1
@save:  lda ZP_SAVE_START,x
        sta zp_save_vbi,x
        dex
        bpl @save

        ; Prevent IRQ re-entering cc65 while deferred VBI is inside C.
        ; Atari VBI is NMI, so IRQs can otherwise interrupt this routine.
        ; cc65 runtime/C stack is not reentrant.
        sei

        ; Switch cc65 software stack to a private VBI stack before calling C.
        ; Even though we restore zero-page state, sharing the same software
        ; stack with interrupted foreground C code can overwrite active frames.
        lda c_sp
        sta saved_sp
        lda c_sp+1
        sta saved_sp+1
        lda #<(vbi_cstack + VBI_CSTACK_SIZE)
        sta c_sp
        lda #>(vbi_cstack + VBI_CSTACK_SIZE)
        sta c_sp+1

        ; Call C tick function
        jsr _mod_vbi_tick

        ; Restore foreground cc65 software stack.
        lda saved_sp
        sta c_sp
        lda saved_sp+1
        sta c_sp+1

        cli

        lda #0
        sta COLBK

        ; Restore cc65 zero page temporaries
        ldx #ZP_SAVE_LEN-1
@rest:  lda zp_save_vbi,x
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

        ; Mark CPU busy while loop IRQ work is executing.
        lda #BUSY_COLOR_IRQ
        sta COLBK

        ; It's ours - save remaining regs only (fast asm path does not call C)
        txa
        pha
        tya
        pha

        jsr _pokeymax_loop_irq_fast

        lda #0
        sta COLBK

        ; loop handler clears IRQACT internally

        pla
        tay
        pla
        tax

@chain:
        pla                     ; restore A saved at entry
        nop
        jmp (old_irq_lo)        ; chain to previous handler
.endproc
