;-----------------------------------------------------------------------------
; adpcm_encode_block.s
;
; uint16_t __fastcall__ adpcm_encode_block(const int8_t *src,
;                                              uint16_t pcm_len,
;                                              uint8_t *dst,
;                                              ADPCMState *state);
;
; cc65 __fastcall__ convention:
;   last argument (state) arrives in AX, A=lo, X=hi
;   stack top to bottom: dst pointer, pcm_len, src pointer
;
; This is a hand-written 6502 implementation of the encoder in adpcm.c.
; It packs samples high-nibble first, matching PokeyMAX RAM format.
;-----------------------------------------------------------------------------

        .export _adpcm_encode_block

        .import popax
        .import _ima_step_table
        .import _ima_index_table

        .segment "LOWCODE"

_adpcm_encode_block:
        ; Save fastcall state pointer
        sta adpcm_state_ptr
        stx adpcm_state_ptr+1

        ; Pop dst pointer
        jsr popax
        sta adpcm_dst_ptr
        stx adpcm_dst_ptr+1

        ; Pop pcm_len
        jsr popax
        sta len
        stx len+1

        ; Pop src pointer
        jsr popax
        sta adpcm_src_ptr
        stx adpcm_src_ptr+1

        ; Pull ADPCMState into locals
        ldy #0
        lda (adpcm_state_ptr),y
        sta pred_lo
        iny
        lda (adpcm_state_ptr),y
        sta pred_hi
        iny
        lda (adpcm_state_ptr),y
        sta step_index

        lda #0
        sta out_count
        sta out_count+1
        sta phase             ; 0 = next nibble goes high; 1 = low pending

@sample_loop:
        lda len
        ora len+1
        beq @finish

        ldy #0
        lda (adpcm_src_ptr),y
        jsr @encode_one       ; result in nibble

        ; src++
        inc adpcm_src_ptr
        bne @src_ok
        inc adpcm_src_ptr+1
@src_ok:

        ; --len
        lda len
        bne @len_lo_nonzero
        dec len+1
@len_lo_nonzero:
        dec len

        lda phase
        bne @pack_low

        ; high nibble first
        lda nibble
        asl a
        asl a
        asl a
        asl a
        sta packed
        lda #1
        sta phase
        jmp @sample_loop

@pack_low:
        lda packed
        ora nibble
        ldy #0
        sta (adpcm_dst_ptr),y
        jsr @inc_dst_out
        lda #0
        sta phase
        jmp @sample_loop

@finish:
        ; Odd input length: flush pending high nibble with zero low nibble
        lda phase
        beq @store_state
        lda packed
        ldy #0
        sta (adpcm_dst_ptr),y
        jsr @inc_dst_out

@store_state:
        ; Write final ADPCMState back
        ldy #0
        lda pred_lo
        sta (adpcm_state_ptr),y
        iny
        lda pred_hi
        sta (adpcm_state_ptr),y
        iny
        lda step_index
        sta (adpcm_state_ptr),y

        lda out_count
        ldx out_count+1
        rts

;-----------------------------------------------------------------------------
; Increment dst pointer and 16-bit return byte count.
;-----------------------------------------------------------------------------
@inc_dst_out:
        inc adpcm_dst_ptr
        bne @dst_ok
        inc adpcm_dst_ptr+1
@dst_ok:
        inc out_count
        bne @out_ok
        inc out_count+1
@out_ok:
        rts

;-----------------------------------------------------------------------------
; Encode one signed 8-bit PCM sample in A.
; Updates pred_lo/pred_hi/step_index.  Returns nibble in nibble.
;-----------------------------------------------------------------------------
@encode_one:
        sta pcm_hi            ; pcm16 = sample << 8, so low byte is zero

        ; step = ima_step_table[step_index]
        lda step_index
        asl a
        tay
        lda _ima_step_table,y
        sta step_lo
        iny
        lda _ima_step_table,y
        sta step_hi

        ; step_half = step >> 1
        lda step_hi
        lsr a
        sta step_half_hi
        lda step_lo
        ror a
        sta step_half_lo

        ; step_qtr = step_half >> 1
        lda step_half_hi
        lsr a
        sta step_qtr_hi
        lda step_half_lo
        ror a
        sta step_qtr_lo

        ; pred_delta = step >> 3 = step_qtr >> 1
        lda step_qtr_hi
        lsr a
        sta delta_hi
        lda step_qtr_lo
        ror a
        sta delta_lo

        ; Signed compare pcm16 < predictor.
        lda pcm_hi
        eor pred_hi
        bmi @signs_differ

        lda pcm_hi
        cmp pred_hi
        bcc @negative_delta
        bne @positive_delta
        lda #0
        cmp pred_lo
        bcc @negative_delta
        jmp @positive_delta

@signs_differ:
        lda pcm_hi
        bmi @negative_delta
        ; else pcm positive, predictor negative

@positive_delta:
        lda #0
        sta nibble
        ; diff = pcm16 - predictor
        sec
        lda #0
        sbc pred_lo
        sta diff_lo
        lda pcm_hi
        sbc pred_hi
        sta diff_hi
        jmp @quantise

@negative_delta:
        lda #8
        sta nibble
        ; diff = predictor - pcm16
        sec
        lda pred_lo
        sbc #0
        sta diff_lo
        lda pred_hi
        sbc pcm_hi
        sta diff_hi

@quantise:
        ; if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
        lda diff_hi
        cmp step_hi
        bcc @skip_bit4
        bne @take_bit4
        lda diff_lo
        cmp step_lo
        bcc @skip_bit4
@take_bit4:
        lda nibble
        ora #4
        sta nibble
        sec
        lda diff_lo
        sbc step_lo
        sta diff_lo
        lda diff_hi
        sbc step_hi
        sta diff_hi
        clc
        lda delta_lo
        adc step_lo
        sta delta_lo
        lda delta_hi
        adc step_hi
        sta delta_hi
@skip_bit4:

        ; if (diff >= step_half) { nibble |= 2; diff -= half; delta += half; }
        lda diff_hi
        cmp step_half_hi
        bcc @skip_bit2
        bne @take_bit2
        lda diff_lo
        cmp step_half_lo
        bcc @skip_bit2
@take_bit2:
        lda nibble
        ora #2
        sta nibble
        sec
        lda diff_lo
        sbc step_half_lo
        sta diff_lo
        lda diff_hi
        sbc step_half_hi
        sta diff_hi
        clc
        lda delta_lo
        adc step_half_lo
        sta delta_lo
        lda delta_hi
        adc step_half_hi
        sta delta_hi
@skip_bit2:

        ; if (diff >= step_qtr) { nibble |= 1; delta += qtr; }
        lda diff_hi
        cmp step_qtr_hi
        bcc @skip_bit1
        bne @take_bit1
        lda diff_lo
        cmp step_qtr_lo
        bcc @skip_bit1
@take_bit1:
        lda nibble
        ora #1
        sta nibble
        clc
        lda delta_lo
        adc step_qtr_lo
        sta delta_lo
        lda delta_hi
        adc step_qtr_hi
        sta delta_hi
@skip_bit1:

        ; Update predictor with sign-overflow clamp.
        lda nibble
        and #8
        bne @apply_negative

        ; new_pred = predictor + delta;
        ; C test is: if (new_pred < predictor) clamp +32767.
        ; This is a full signed comparison, not just sign-crossing: delta can
        ; exceed 32767 at high step indices, so the wrapped result may remain
        ; negative while still being less than the old negative predictor.
        clc
        lda pred_lo
        adc delta_lo
        sta new_lo
        lda pred_hi
        adc delta_hi
        sta new_hi

        lda new_hi
        eor pred_hi
        bmi @add_signs_differ
        ; Same sign: unsigned byte compare preserves signed ordering.
        lda new_hi
        cmp pred_hi
        bcc @add_overflow
        bne @add_no_overflow
        lda new_lo
        cmp pred_lo
        bcc @add_overflow
        jmp @add_no_overflow
@add_signs_differ:
        ; Different signs: new < old only when new is negative.
        lda new_hi
        bmi @add_overflow
        jmp @add_no_overflow
@add_overflow:
        lda #$ff              ; clamp +32767
        sta pred_lo
        lda #$7f
        sta pred_hi
        jmp @update_index
@add_no_overflow:
        lda new_lo
        sta pred_lo
        lda new_hi
        sta pred_hi
        jmp @update_index

@apply_negative:
        ; new_pred = predictor - delta;
        ; C test is: if (new_pred > predictor) clamp -32768.
        ; Again this must be a full signed comparison.
        sec
        lda pred_lo
        sbc delta_lo
        sta new_lo
        lda pred_hi
        sbc delta_hi
        sta new_hi

        lda new_hi
        eor pred_hi
        bmi @sub_signs_differ
        ; Same sign: clamp if new > old.
        lda new_hi
        cmp pred_hi
        bcc @sub_no_underflow
        bne @sub_underflow
        lda new_lo
        cmp pred_lo
        bcc @sub_no_underflow
        beq @sub_no_underflow
        jmp @sub_underflow
@sub_signs_differ:
        ; Different signs: new > old only when new is positive.
        lda new_hi
        bpl @sub_underflow
        jmp @sub_no_underflow
@sub_underflow:
        lda #$00              ; clamp -32768
        sta pred_lo
        lda #$80
        sta pred_hi
        jmp @update_index
@sub_no_underflow:
        lda new_lo
        sta pred_lo
        lda new_hi
        sta pred_hi

@update_index:
        ldy nibble
        lda step_index
        clc
        adc _ima_index_table,y
        bmi @idx_underflow         ; A < 0 → clamp to 0
        cmp #89
        bcc @idx_store             ; in range → store unchanged
        lda #88                    ; A >= 89 → clamp
        bne @idx_store             ; always-taken (88 != 0)
@idx_underflow:
        lda #0
@idx_store:
        sta step_index
        ; nibble already in low four bits
        rts

        .segment "ZEROPAGE"

; Private ZP pointers for the encoder.  Do not use cc65 ptr1/ptr2/ptr3
; here: this routine can run while the player/VBI/IRQ machinery is active,
; and those runtime temporaries are shared scratch.
adpcm_src_ptr:      .res 2
adpcm_dst_ptr:      .res 2
adpcm_state_ptr:    .res 2

        .segment "LOWBSS"

len:            .res 2
out_count:      .res 2
phase:          .res 1
packed:         .res 1
nibble:         .res 1
pcm_hi:         .res 1
pred_lo:        .res 1
pred_hi:        .res 1
step_index:     .res 1
step_lo:        .res 1
step_hi:        .res 1
step_half_lo:   .res 1
step_half_hi:   .res 1
step_qtr_lo:    .res 1
step_qtr_hi:    .res 1
diff_lo:        .res 1
diff_hi:        .res 1
delta_lo:       .res 1
delta_hi:       .res 1
new_lo:         .res 1
new_hi:         .res 1
