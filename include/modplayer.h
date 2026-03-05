/*
 * modplayer.h - MOD player engine for PokeyMAX
 *
 * Supports ProTracker 4-channel MOD files.
 * Runs from a deferred VBI (vertical blank interrupt) at 50Hz (PAL)
 * or 60Hz (NTSC), with sub-tick via PokeyMAX sample-end IRQs.
 *
 * Effects implemented:
 *   0: Arpeggio
 *   1: Slide up (portamento up)
 *   2: Slide down (portamento down)
 *   3: Tone portamento (slide to note) -- basic version
 *   4: Vibrato -- basic version
 *   C: Set volume
 *   D: Pattern break
 *   E1: Fine slide up
 *   E2: Fine slide down
 *   E6: Pattern loop
 *   EC: Note cut
 *   ED: Note delay
 *   F: Set speed / set BPM
 */

#ifndef MODPLAYER_H
#define MODPLAYER_H

#include <stdint.h>
#include "mod_format.h"

/* -------------------------------------------------------
 * Configuration
 * ------------------------------------------------------- */
#define MOD_USE_PAL         1   /* 1=PAL 50Hz VBI, 0=NTSC 60Hz VBI */

/* Amiga Paula clock for period→frequency conversion
 * PAL:  3546895 Hz / 2 = 1773447  (per channel)
 * NTSC: 3579545 Hz / 2 = 1789772  */
#if MOD_USE_PAL
  #define PAULA_CLOCK       3546895UL
  #define VBI_HZ            50
#else
  #define PAULA_CLOCK       3579545UL
  #define VBI_HZ            60
#endif

/* PokeyMAX core clock = 2*phi2 */
#define POKEYMAX_CLOCK      (PAULA_CLOCK * 2UL)

/*
 * Ticks per row = speed (set by Fx with x<32)
 * Rows per beat = 4 (fixed in ProTracker)
 * VBI calls per tick = VBI_HZ * 60 / (BPM * 24)
 *   = VBI_HZ * 60 / (BPM * 24)
 *
 * At 125 BPM, speed 6, PAL:
 *   ticks_per_row = 6
 *   VBI per tick = 50*60 / (125*24) = 3000/3000 = 1  (exactly 1 VBI per tick)
 *
 * We implement a fractional tick counter for non-standard BPM.
 */

/* -------------------------------------------------------
 * API
 * ------------------------------------------------------- */

/*
 * mod_read_row()
 * Read one row (16 bytes) from disk into mod_row_buf[].
 * Called by the player engine on each new row.
 */
uint8_t mod_read_row(uint8_t pattern, uint8_t row);
extern uint8_t mod_row_buf[MOD_CHANNELS * 4];

/*
 * mod_play() / mod_stop() / mod_pause()
 */
void mod_play(void);
void mod_stop(void);
void mod_pause(void);

/*
 * mod_vbi_tick()
 * Call from deferred VBI ISR. Advances the player by one VBI frame.
 * This function advances ticks as needed (possibly 0 or multiple ticks
 * per VBI depending on BPM).
 */
void mod_vbi_tick(void);

/*
 * mod_set_volume()
 * Master volume 0-63. Applied to all channels.
 */
void mod_set_volume(uint8_t vol);

/*
 * mod_sample_irq_service()
 * Fast service function for PokeyMAX sample-end IRQs.
 * Returns 1 if a sample IRQ was pending and handled, 0 if not pending.
 */
uint8_t mod_sample_irq_service(void);

#endif /* MODPLAYER_H */
