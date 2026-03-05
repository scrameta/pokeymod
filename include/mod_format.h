/*
 * mod_format.h - ProTracker / Amiga MOD file format structures
 *
 * Supports: M.K. (4 channel), M!K! (4 ch extended), FLT4 (4 ch).
 * We play 4 channels mapping to PokeyMAX sample player channels 1-4.
 */

#ifndef MOD_FORMAT_H
#define MOD_FORMAT_H

#include <stdint.h>

/* -------------------------------------------------------
 * Limits
 * ------------------------------------------------------- */
#define MOD_MAX_SAMPLES     31
#define MOD_MAX_PATTERNS    128
#define MOD_TITLE_LEN       20
#define MOD_SAMPLE_NAME_LEN 22
#define MOD_ROWS_PER_PAT    64
#define MOD_CHANNELS        4


/* -------------------------------------------------------
 * Derived Limits
 * ------------------------------------------------------- */
#define PAT_BYTES   (MOD_ROWS_PER_PAT * MOD_CHANNELS * 4)   /* 1024 */

/* -------------------------------------------------------
 * Sample header (in MOD file, big-endian)
 * ------------------------------------------------------- */
typedef struct {
    char     name[MOD_SAMPLE_NAME_LEN];
    uint16_t length_be;      /* length in words (big-endian) */
    uint8_t  finetune;       /* lower 4 bits, signed (-8..+7) */
    uint8_t  volume;         /* 0-64 */
    uint16_t loop_start_be;  /* word offset */
    uint16_t loop_len_be;    /* in words; <=1 means no loop */
} MODSampleHdr;

/* -------------------------------------------------------
 * In-memory decoded sample info
 * ------------------------------------------------------- */
typedef struct {
    uint16_t length;         /* in bytes (decoded from words) */
    uint8_t  volume;         /* 0-64 */
    uint16_t loop_start;     /* byte offset */
    uint16_t loop_len;       /* in bytes; 0 or 2 = no loop */
    uint16_t pokeymax_addr;  /* address in PokeyMAX block RAM */
    uint16_t pokeymax_len;   /* actual length stored (may be ADPCM compressed) */
    uint8_t  flags;          /* see SI_* macros below */
} SampleInfo;
/* flags byte layout:
 * bits 7:4  finetune bias (stored as finetune+8, range 0..15)
 * bits 3:2  downsample shift (0=none, 1=half-rate, 2=quarter-rate)
 * bits 1:0  sample type: 0=PCM, 1=PCM+loop, 2=ADPCM
 */
#define SI_STYPE_PCM       0x00
#define SI_STYPE_PCM_LOOP  0x01
#define SI_STYPE_ADPCM     0x02

#define SI_HAS_LOOP(si)    (((si)->flags & 0x03) == SI_STYPE_PCM_LOOP)
#define SI_IS_ADPCM(si)    (((si)->flags & 0x03) == SI_STYPE_ADPCM)
#define SI_DS_SHIFT(si)    (((si)->flags >> 2) & 0x03)
#define SI_FINETUNE(si)    ((int8_t)(((si)->flags >> 4) & 0x0F) - 8)

/* -------------------------------------------------------
 * A single note in a pattern (4 bytes in MOD file)
 *
 * Byte layout (big-endian):
 *   [0] = (sample_hi << 4) | period_hi_hi_hi_hi  -- upper nibble = sample hi, lower = period bits 8-11
 *   [1] = period_lo                               -- period bits 0-7
 *   [2] = (sample_lo << 4) | effect_cmd           -- upper nibble = sample lo, lower = effect
 *   [3] = effect_param
 * ------------------------------------------------------- */
typedef struct {
    uint8_t raw[4];
} MODNote;

/* Decoded note fields */
typedef struct {
    uint8_t  sample;      /* 0 = no sample, 1-31 */
    uint16_t period;      /* Amiga period value */
    uint8_t  effect;      /* 0-F */
    uint8_t  param;       /* effect parameter */
} Note;

/* Decode a raw MOD note - implemented in tables.c */
void mod_decode_note(const MODNote *raw, Note *out);

/* -------------------------------------------------------
 * Amiga period table (octaves 1-3, notes C-B)
 * Finetune = 0 values. Index 0=C-1, 1=C#-1 ... 11=B-1, 12=C-2 etc.
 * ------------------------------------------------------- */
#define NUM_PERIODS 36

extern const uint16_t amiga_periods[NUM_PERIODS];
/* fine-tune tables: [finetune+8][note] – we compute on the fly to save RAM */

/* -------------------------------------------------------
 * Effect numbers we implement
 * ------------------------------------------------------- */
#define FX_ARPEGGIO         0x0
#define FX_SLIDE_UP         0x1
#define FX_SLIDE_DOWN       0x2
#define FX_TONE_PORTAMENTO  0x3
#define FX_VIBRATO          0x4
#define FX_SET_VOLUME       0xC
#define FX_PATTERN_BREAK    0xD
#define FX_EXTENDED         0xE
#define FX_SET_SPEED        0xF

/* Extended (0xE) sub-effects */
#define EFX_FINE_SLIDE_UP   0x1
#define EFX_FINE_SLIDE_DOWN 0x2
#define EFX_LOOP            0x6
#define EFX_RETRIGGER       0x9
#define EFX_FINE_VOL_UP     0xA
#define EFX_FINE_VOL_DOWN   0xB
#define EFX_NOTE_CUT        0xC
#define EFX_NOTE_DELAY      0xD
#define EFX_PATTERN_DELAY   0xE

#endif /* MOD_FORMAT_H */
