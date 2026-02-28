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
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

void pokeymax_loop_handler(void)
{
    unsigned char active;
    unsigned char ch;
    unsigned char bit;

    active = PEEK(REG_IRQACT);
    POKE(REG_IRQACT, 0x00);     /* clear all IRQ flags immediately */

    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        bit = (unsigned char)(1u << ch);
        if (!(active & bit)) continue;

        {
            ChanState *cs = &mod.chan[ch];
            uint8_t    hw = (uint8_t)(ch + 1u);

            if (!cs->active) continue;

            if (cs->has_loop && cs->loop_len > 2u) {
                /* Re-arm with loop region. Addresses are in bytes. */
                uint16_t loop_addr;
                uint16_t loop_len;

                if (cs->is_adpcm) {
                    /* ADPCM: 2 samples per byte, so byte addr = sample_offset/2 */
                    loop_addr = cs->sam_addr + (cs->loop_start >> 1);
                    loop_len  = cs->loop_len;   /* length in samples */
                } else {
                    loop_addr = cs->sam_addr + cs->loop_start;
                    loop_len  = cs->loop_len;
                }

                pokeymax_channel_trigger(hw, loop_addr, loop_len);
            } else {
                /* One-shot: fully stop channel (DMA+IRQ off) to avoid IRQ storms
                 * on already-ended samples. Just zeroing volume can leave the
                 * sample-end IRQ reasserting continuously on some lengths. */
                cs->active = 0;
                pokeymax_channel_stop(hw);
                POKE(REG_CHANSEL, hw);
                POKE(REG_VOL, 0);
            }
        }
    }
}
