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

/* Default BPM and speed (ProTracker defaults) */
#define DEFAULT_BPM         125
#define DEFAULT_SPEED       6

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
 * Channel state
 * ------------------------------------------------------- */
typedef struct {
    /* Current note */
    uint8_t  sample_num;    /* 1-31, 0=none */
    uint16_t period;        /* current Amiga period */
    uint16_t target_period; /* for tone portamento */
    uint8_t  volume;        /* 0-64 */
    uint8_t  hw_vol;        /* scaled to 0-63 for PokeyMAX */

    /* Effects */
    uint8_t  effect;
    uint8_t  param;

    /* Vibrato */
    uint8_t  vib_pos;       /* position 0-63 in sine table */
    uint8_t  vib_speed;
    uint8_t  vib_depth;

    /* Arpeggio */
    uint8_t  arp_tick;

    /* Note cut/delay */
    uint8_t  note_delay_ticks;
    uint8_t  note_cut_tick;

    /* Portamento */
    uint8_t  port_speed;

    /* Pattern loop */
    uint8_t  loop_row;
    uint8_t  loop_count;

    /* Sample playback state */
    uint16_t sam_addr;      /* address in PokeyMAX RAM */
    uint16_t sam_len;       /* sample length in samples */
    uint16_t loop_start;    /* loop start in samples */
    uint16_t loop_len;      /* loop length in samples */
    uint8_t  has_loop;
    uint8_t  is_adpcm;
    uint8_t  is_8bit;
    uint8_t  triggered;     /* 1 if we just triggered a new note this row */
    uint8_t  active;        /* 1 if channel has a sample loaded */
} ChanState;

/* -------------------------------------------------------
 * Player state
 * ------------------------------------------------------- */
typedef struct {
    /* Song data */
    uint8_t  order_table[MOD_MAX_PATTERNS];
    uint8_t  song_length;
    uint8_t  num_patterns;

    /* Playback position */
    uint8_t  order_pos;            /* current position in order table */
    uint8_t  row;                  /* current row 0-63 */
    uint8_t  tick;                 /* current tick within row */

    /* Timing */
    uint8_t  speed;                /* ticks per row */
    uint16_t bpm;
    uint16_t vbi_accum;            /* fractional VBI accumulator (16.16 fixed pt) */
    uint16_t vbi_per_tick_int;     /* integer VBIs per tick */
    uint16_t vbi_per_tick_frac;    /* fractional part (256ths) */
    uint8_t  vbi_frac_accum;       /* running fraction */

    /* Pattern break/jump */
    uint8_t  pattern_break;
    uint8_t  break_row;
    uint8_t  jump_order;
    uint8_t  do_jump;

    /* Pattern delay (EE effect) */
    uint8_t  pattern_delay;
    uint8_t  pattern_delay_count;

    /* Channel states */
    ChanState chan[MOD_CHANNELS];

    /* Sample info (loaded at startup) */
    SampleInfo samples[MOD_MAX_SAMPLES + 1];   /* 1-indexed */

    /* Status */
    uint8_t  playing;
    uint8_t  loop_song;     /* restart at end if 1 */
} ModPlayer;

/* -------------------------------------------------------
 * Global player instance (single player)
 * ------------------------------------------------------- */
extern ModPlayer mod;

typedef struct {
    void (*begin)(void *ctx, uint32_t source_total);
    void (*update)(void *ctx,
                   const SampleInfo *si,
                   uint16_t sample_index,
                   uint16_t total_samples,
                   uint32_t source_loaded,
                   uint32_t source_total,
                   uint32_t stored_loaded,
                   uint8_t skipped);
    void (*end)(void *ctx);
    void *ctx;
} ModLoadProgressPlugin;

/* -------------------------------------------------------
 * API
 * ------------------------------------------------------- */

/*
 * mod_load()
 * Open a MOD file from disk, stream samples to PokeyMAX block RAM.
 * The file is kept open during playback for row-by-row pattern reads.
 * Returns 0 on success, non-zero on error.
 */
uint8_t mod_load(const char *filename);

/*
 * mod_read_row()
 * Read one row (16 bytes) from disk into mod_row_buf[].
 * Called by the player engine on each new row.
 */
uint8_t mod_read_row(uint8_t pattern, uint8_t row);
uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr);
extern uint8_t mod_row_buf[MOD_CHANNELS * 4];

/*
 * mod_file_close()
 * Close the MOD file (call after playback ends).
 */
void mod_file_close(void);

/* Optional minimal hook to replace disk pattern fetch backend.
 * Pass NULL to restore default disk fetch implementation.
 */
void mod_set_pattern_fetch_fn(uint8_t (*fetch_fn)(uint8_t pattern_num, uint8_t *dst));

/* Optional: configure a bank-switched pattern source (Atari XL/XE).
 * Patterns are stored in a 16KB bank window selected through PIA PORTB.
 * first_bank = first bank number used by pattern storage.
 * bank_count = number of consecutive 16KB banks available (0 disables).
 */
void mod_set_pattern_bank_range(uint8_t first_bank, uint8_t bank_count);

/*
 * Load progress/status plugin for mod_load().
 * Pass NULL to disable plugin output entirely.
 */
void mod_set_load_progress_plugin(const ModLoadProgressPlugin *plugin);
extern const ModLoadProgressPlugin mod_default_load_progress_plugin;

/*
 * mod_pattern_advance()
 * Called from VBI when order position changes. Swaps pattern buffers
 * and flags the main loop to prefetch the next pattern. No disk I/O.
 */
void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next);

/*
 * mod_prefetch_next_pattern()
 * Called from the MAIN LOOP (never from VBI). Reads the next pattern
 * from disk into the standby buffer. Call when mod_need_prefetch != 0.
 * Returns 0 on success.
 */
uint8_t mod_prefetch_next_pattern(void);

/*
 * mod_need_prefetch
 * Set by mod_pattern_advance() when a new pattern needs loading.
 * Cleared by mod_prefetch_next_pattern(). Polled by the main loop.
 */
extern uint8_t mod_need_prefetch;

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

/*
 * mod_get_row() / mod_get_order()
 * Return current playback position for display.
 */
uint8_t mod_get_row(void);
uint8_t mod_get_order(void);

#endif /* MODPLAYER_H */
