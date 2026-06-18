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
#define PAL_PAULA_CLOCK     3546895UL
#define NTSC_PAULA_CLOCK    3579545UL
#define PAL_POKEY_CLOCK      (PAL_PAULA_CLOCK / 2UL)
#define NTSC_POKEY_CLOCK     (NTSC_PAULA_CLOCK / 2UL)
#define PAL_VBI_HZ          50
#define NTSC_VBI_HZ         60

/* PokeyMAX core clock = 2*phi2.  Sample period conversion uses the
 * PAL Paula-compatible clock; tracker tick timing is selected at runtime. */
#define POKEYMAX_CLOCK      PAL_PAULA_CLOCK

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

/* Detect Atari video standard from GTIA PAL register ($D014).
 * Returns PAL_VBI_HZ or NTSC_VBI_HZ. */
uint8_t mod_detect_vbi_hz(void);

/*
 * mod_vbi_tick()
 * Call from deferred VBI ISR. Advances the player by one VBI frame.
 * This function advances ticks as needed (possibly 0 or multiple ticks
 * per VBI depending on BPM).
 */
void mod_vbi_tick(void);

/* Timer IRQ entry point.  Runs exactly one MOD tick; the POKEY timer period
 * is updated when an F20+ BPM command changes mod.bpm. */
void mod_timer_tick(void);

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
