;
; cio_call.s — call CIO with IOCB #6, return status in A
;
        .export _cio_call6

CIOV = $E456
IOCB6_OFS = $60    ; IOCB #6 = 6 * 16

.code

.proc _cio_call6
        ldx #IOCB6_OFS
        jsr CIOV
        tya             ; status is in Y after CIO call
        ldx #0          ; high byte = 0 (return uint8_t)
        rts
.endproc
