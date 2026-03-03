        .export _pokeymax_loop_irq_fast

        .import _pokeymax_mod_chan_base
        .import _pokeymax_dma_shadow
        .import _pokeymax_irqen_shadow

; PokeyMAX regs
REG_CHANSEL = $D288
REG_ADDRL   = $D289
REG_ADDRH   = $D28A
REG_LENL    = $D28B
REG_LENH    = $D28C
REG_DMA     = $D290
REG_IRQEN   = $D291
REG_IRQACT  = $D292
REG_VOL     = $D28F

; ChanState field offsets/stride (cc65 layout checked in loop_handler.c)
CHAN_STRIDE       = 33
CS_SAM_ADDR       = 20
CS_LOOP_START     = 24
CS_LOOP_LEN       = 26
CS_HAS_LOOP       = 28
CS_IS_ADPCM       = 29
CS_ACTIVE         = 32

; temps (BSS, not reentrant-safe by design: IRQ path only)
.segment "ZEROPAGE"
cs_ptr:      .res 2

.bss
irq_bits:    .res 1
bitmask:     .res 1
chan_num:    .res 1
addr_tmp:    .res 2
len_tmp:     .res 2
nbit_tmp:    .res 1

.code
.proc _pokeymax_loop_irq_fast
        lda REG_IRQACT
        sta irq_bits
        lda #$00
        sta REG_IRQACT              ; clear all pending quickly

        lda #1
        sta bitmask
        lda #1
        sta chan_num

@ch_loop:
        lda irq_bits
        and bitmask
        bne :+
        jmp @next_ch
:
        ; cs_ptr = pokeymax_mod_chan_base + (chan_num-1)*CHAN_STRIDE
        lda _pokeymax_mod_chan_base
        sta cs_ptr
        lda _pokeymax_mod_chan_base+1
        sta cs_ptr+1

        ldx chan_num
        dex
@add_stride:
        cpx #0
        beq @have_ptr
        clc
        lda cs_ptr
        adc #CHAN_STRIDE
        sta cs_ptr
        bcc :+
        inc cs_ptr+1
:       dex
        jmp @add_stride

@have_ptr:
        ldy #CS_ACTIVE
        lda (cs_ptr),y
        bne :+
        jmp @next_ch
:
        ldy #CS_HAS_LOOP
        lda (cs_ptr),y
        bne :+
        jmp @oneshot
:

        ; loop_len > 2 ?
        ldy #CS_LOOP_LEN+1
        lda (cs_ptr),y
        bne @do_loop
        ldy #CS_LOOP_LEN
        lda (cs_ptr),y
        cmp #3
        bcs :+
        jmp @oneshot
:

@do_loop:
        ; addr_tmp = sam_addr + loop_start (or + loop_start>>1 for ADPCM)
        ldy #CS_SAM_ADDR
        lda (cs_ptr),y
        sta addr_tmp
        iny
        lda (cs_ptr),y
        sta addr_tmp+1

        ldy #CS_IS_ADPCM
        lda (cs_ptr),y
        beq @pcm_addr
        ; add (loop_start >> 1)
        ldy #CS_LOOP_START+1
        lda (cs_ptr),y
        lsr a
        sta len_tmp+1              ; reuse as shifted high
        ldy #CS_LOOP_START
        lda (cs_ptr),y
        ror a
        sta len_tmp
        clc
        lda addr_tmp
        adc len_tmp
        sta addr_tmp
        lda addr_tmp+1
        adc len_tmp+1
        sta addr_tmp+1
        jmp @load_len

@pcm_addr:
        clc
        ldy #CS_LOOP_START
        lda addr_tmp
        adc (cs_ptr),y
        sta addr_tmp
        iny
        lda addr_tmp+1
        adc (cs_ptr),y
        sta addr_tmp+1

@load_len:
        ldy #CS_LOOP_LEN
        lda (cs_ptr),y
        sta len_tmp
        iny
        lda (cs_ptr),y
        sta len_tmp+1

        ; nbit = ~bitmask & $0f
        lda bitmask
        eor #$ff
        and #$0f
        sta nbit_tmp

        ; CHANSEL = chan_num ; write addr/len (len-1)
        lda chan_num
        sta REG_CHANSEL
        lda addr_tmp
        sta REG_ADDRL
        lda addr_tmp+1
        sta REG_ADDRH
        sec
        lda len_tmp
        sbc #1
        sta REG_LENL
        lda len_tmp+1
        sbc #0
        sta REG_LENH

        ; mask IRQ bit in shadow + hw
        lda _pokeymax_irqen_shadow
        and nbit_tmp
        sta _pokeymax_irqen_shadow
        sta REG_IRQEN

        ; DMA 1->0->1 using shadow
        lda _pokeymax_dma_shadow
        and nbit_tmp
        sta _pokeymax_dma_shadow
        sta REG_DMA
        lda _pokeymax_dma_shadow
        ora bitmask
        sta _pokeymax_dma_shadow
        sta REG_DMA

        ; drop synthetic edge IRQ for this channel, then unmask irq again
        lda nbit_tmp
        sta REG_IRQACT
        lda _pokeymax_irqen_shadow
        ora bitmask
        sta _pokeymax_irqen_shadow
        sta REG_IRQEN
        jmp @next_ch

@oneshot:
        ; cs->active = 0
        ldy #CS_ACTIVE
        lda #0
        sta (cs_ptr),y

        ; shadow &= nbit ; hw write DMA/IRQEN
        lda bitmask
        eor #$ff
        and #$0f
        sta nbit_tmp
        lda _pokeymax_dma_shadow
        and nbit_tmp
        sta _pokeymax_dma_shadow
        sta REG_DMA
        lda _pokeymax_irqen_shadow
        and nbit_tmp
        sta _pokeymax_irqen_shadow
        sta REG_IRQEN

        ; volume=0 for selected channel
        lda chan_num
        sta REG_CHANSEL
        lda #0
        sta REG_VOL

@next_ch:
        asl bitmask
        inc chan_num
        lda chan_num
        cmp #5
        bcs :+
        jmp @ch_loop
:
        rts
.endproc
