/*
 * loop_handler.c - PokeyMAX sample-end IRQ handler
 *
 * Called from our_irq (vbi_handler.s) when SAM_IRQACT is non-zero.
 * Re-arms looping channels with the loop region address/length.
 * For one-shot samples: silences the channel.
 *
 * PokeyMAX buffers addr+len: writing them takes effect at the next
 * sample boundary, giving seamless looping.
 */

#include <stdint.h>
#include <stddef.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "mod_struct.h"

#ifdef __CC65__
typedef char _assert_chan_size    [(sizeof(ChanState)==33)                  ?1:-1];
typedef char _assert_cs_sam_addr  [(offsetof(ChanState, sam_addr)==20)      ?1:-1];
typedef char _assert_cs_loop_start[(offsetof(ChanState, loop_start)==24)    ?1:-1];
typedef char _assert_cs_loop_len  [(offsetof(ChanState, loop_len)==26)      ?1:-1];
typedef char _assert_cs_has_loop  [(offsetof(ChanState, has_loop)==28)      ?1:-1];
typedef char _assert_cs_is_adpcm  [(offsetof(ChanState, is_adpcm)==29)      ?1:-1];
typedef char _assert_cs_active    [(offsetof(ChanState, active)==32)        ?1:-1];
#endif

#ifdef __CC65__
/* Exported for 6502 asm IRQ path so it does not depend on ModPlayer layout */
ChanState *pokeymax_mod_chan_base = &mod.chan[0];
#endif
