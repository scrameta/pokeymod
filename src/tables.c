/*
 * tables.c - Amiga MOD period table and vibrato sine table
 */

#include <stdint.h>
#include "mod_format.h"

/*
 * Amiga periods for finetune=0, octaves 1-3 (standard MOD range).
 * Index: 0=C-1 ... 11=B-1, 12=C-2 ... 23=B-2, 24=C-3 ... 35=B-3
 */
const uint16_t amiga_periods[NUM_PERIODS] = {
    /* Octave 1 */
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    /* Octave 2 */
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    /* Octave 3 */
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113
};

/*
 * Finetune adjustment table.
 * amiga_periods_ft[finetune+8][note] for notes 0-35.
 * Full 9*36 table. finetune -8..+7 stored as index 0..15.
 * Only octave 2 (indices 12-23) shown here for space; the player
 * interpolates via the recalculation formula for other ranges.
 *
 * Actually, to save RAM on a 6502 system we compute the fine-tuned
 * period dynamically: period_ft = period * finetune_factor
 * where finetune_factor is from a small 16-entry table.
 *
 * Factors are (1/2^(finetune/96)) approximations stored as period deltas.
 * In practice the simplest approach: use full period table (ProTracker uses
 * a 16-finetune * 36-note = 576 entry table). We store just finetune=0
 * and adjust at runtime using the formula:
 *   adjusted_period = base_period * (1.0 + finetune * -1/96.0)
 * approximated as integer arithmetic.
 *
 * Fine-tune period ratios (8.8 fixed point, multiply base period):
 * Index 0 = finetune -8, index 15 = finetune +7
 */
const uint16_t finetune_ratio[16] = {
    /* -8     -7     -6     -5     -4     -3     -2     -1 */
    0x0109, 0x0107, 0x0105, 0x0104, 0x0102, 0x0101, 0x0100, 0x0100,
    /* +0     +1     +2     +3     +4     +5     +6     +7 */
    0x0100, 0x00FF, 0x00FE, 0x00FE, 0x00FD, 0x00FC, 0x00FA, 0x00F8
};
/* Usage: adjusted = (base * finetune_ratio[ft+8]) >> 8 */

/*
 * Vibrato sine table (64 entries, signed range -128..127).
 * Used by vibrato effect (effect 4).
 */
const int8_t vibrato_sine[64] = {
      0,  24,  49,  74,  97, 120,-115, -95,
    -76, -59, -44, -32, -21, -12,  -6,  -3,
     -1,  -3,  -6, -12, -21, -32, -44, -59,
    -76, -95,-115, 120,  97,  74,  49,  24,
      0, -24, -49, -74, -97,-120, 115,  95,
     76,  59,  44,  32,  21,  12,   6,   3,
      1,   3,   6,  12,  21,  32,  44,  59,
     76,  95, 115,-120, -97, -74, -49, -24
};

