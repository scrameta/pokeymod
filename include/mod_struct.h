#ifndef MODSTRUCT_H
#define MODSTRUCT_H

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
    uint16_t hw_period;     /* speed up */
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
    /* Which file and where are patterns - can load/stream on player side if desired */
    char     filename[64];
    uint32_t pattern_data_offset;
    uint32_t pattern_data_size;

    /* Where we store the data in ram in 4k chunks: XL banks, under OS or main ram */
    uint8_t  banks;
    uint8_t  pattern_portb[64];
    uint16_t pattern_bank_addr[64];
    uint8_t pattern_row_buf[MOD_CHANNELS*4];

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

extern ModPlayer mod;

#endif 
