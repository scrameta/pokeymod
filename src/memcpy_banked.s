;-----------------------------------------------------------------------------
; memcpy_banked.s
;
; void __fastcall__ memcpy_banked(uint8_t *dst, uint8_t *src,
;                                  uint16_t len, uint8_t new_portb,
;                                  uint8_t restore_portb, uint8_t restore_nmien);
;
; cc65 __fastcall__ convention:
;   Last param (restore_nmien) arrives in A register
;   Stack on entry, top to bottom:
;     restore_portb   (1 byte)
;     new_portb       (1 byte)
;     len             (2 bytes)
;     src             (2 bytes)
;     dst             (2 bytes)
;
; Uses cc65 runtime ZP ptr1 (dst) and ptr2 (src) for indirect indexed
; addressing. These are safe because the copy loop makes zero JSR calls,
; so cc65 runtime cannot clobber them mid-loop.
; len and local vars are in LOWBSS (not ZP) since no indirect needed.
;-----------------------------------------------------------------------------

    .export _memcpy_banked

    .import popax               ; pop 16-bit value into X(hi):A(lo)
    .import popa                ; pop 8-bit value into A
    .importzp ptr1              ; $05/$06 - use for dst (indirect indexed)
    .importzp ptr2              ; $07/$08 - use for src (indirect indexed)

; Hardware registers (write-only)
PORTB       = $D301
NMIEN       = $D40E

;-----------------------------------------------------------------------------
    .segment "LOWCODE"

_memcpy_banked:
    ;----------------------------------------------------------
    ; A = restore_nmien on entry (fastcall last param)
    ;----------------------------------------------------------
    sta restore_nmien

    ; pop restore_portb (8-bit)
    jsr popa
    sta restore_portb

    ; pop new_portb (8-bit)
    jsr popa
    sta new_portb

    ; pop len (16-bit)
    jsr popax
    sta bss_len
    stx bss_len+1

    ; pop src (16-bit) into ptr2
    jsr popax
    sta ptr2
    stx ptr2+1

    ; pop dst (16-bit) into ptr1
    jsr popax
    sta ptr1
    stx ptr1+1

    ;----------------------------------------------------------
    ; Disable NMI then IRQ
    ;----------------------------------------------------------
    lda #$00
    sta NMIEN
    sei

    ;----------------------------------------------------------
    ; Switch to new bank
    ;----------------------------------------------------------
    lda new_portb
    sta PORTB

    ;----------------------------------------------------------
    ; Copy loop - no JSR calls, ptr1/ptr2 are safe from cc65
    ; Y stays 0; pointers bumped manually each byte
    ;----------------------------------------------------------
    ldy #$00

@loop:
    lda bss_len
    ora bss_len+1
    beq @done

    lda (ptr2),y            ; load *src
    sta (ptr1),y            ; store *dst

    ; src++ (ptr2)
    inc ptr2
    bne @src_hi_ok
    inc ptr2+1
@src_hi_ok:

    ; dst++ (ptr1)
    inc ptr1
    bne @dst_hi_ok
    inc ptr1+1
@dst_hi_ok:

    ; --len
    lda bss_len
    bne @lo_nonzero
    dec bss_len+1
@lo_nonzero:
    dec bss_len

    jmp @loop

@done:
    ;----------------------------------------------------------
    ; Restore bank then re-enable interrupts
    ;----------------------------------------------------------
    lda restore_portb
    sta PORTB

    cli
    lda restore_nmien
    sta NMIEN

    rts

;-----------------------------------------------------------------------------
    .segment "LOWBSS"

bss_len:        .res 2
new_portb:      .res 1
restore_portb:  .res 1
restore_nmien:  .res 1
