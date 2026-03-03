/*
 * pokeymax_hw.h - PokeyMAX sample player hardware abstraction
 */

#ifndef POKEYMAX_HW_H
#define POKEYMAX_HW_H

#include <stdint.h>
#include "pokeymax.h"

#define POKEYMAX_RAM_SIZE   43008U
#define POKEYMAX_ALLOC_FAIL 0xFFFFU

extern uint16_t pokeymax_ram_ptr;
extern uint8_t pokeymax_samcfg_shadow;
extern uint8_t pokeymax_dma_shadow;
extern uint8_t pokeymax_irqen_shadow;


/* Initialise hardware, reset allocator, silence all channels */
void pokeymax_init(void);

/* Bump allocator */
uint16_t pokeymax_alloc(uint16_t bytes);

/*
 * Write 8-bit SIGNED PCM data to block RAM
 * Use this for all 8-bit PCM samples.
 */
void pokeymax_write_ram(uint16_t addr, const uint8_t *data, uint16_t len);

/*
 * Configure and start a channel from scratch.
 * chan: 1-4
 * addr: address in PokeyMAX block RAM (bytes)
 * len:  sample length in samples
 * period: hw_period = amiga_period  (PokeyMAX clock = 2*PHI2, same as Paula clock)
 * vol: 0-63
 * Toggles DMA 0->1 to trigger immediately.
 */
void pokeymax_channel_setup(uint8_t chan, uint16_t addr, uint16_t len,
                             uint16_t period, uint8_t vol,
                             uint8_t mode_8bit, uint8_t mode_adpcm);

/*
 * Retrigger a channel with new addr/len (used by loop handler).
 * Toggles DMA 1->0->1. Keeps current period and volume.
 */
void pokeymax_channel_trigger(uint8_t chan, uint16_t addr, uint16_t len);

/* Update period + volume without restarting (safe mid-playback) */
void pokeymax_channel_set_period_vol(uint8_t chan, uint16_t period, uint8_t vol);

/* Enable DMA on a single channel without toggling (no retrigger) */
void pokeymax_channel_dma_on(uint8_t chan);

/* Disable DMA + IRQ on a single channel */
void pokeymax_channel_stop(uint8_t chan);

/* Enable IRQ on all 4 channels */
void pokeymax_irq_enable_all(void);

#endif /* POKEYMAX_HW_H */
