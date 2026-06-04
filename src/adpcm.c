/*
 * adpcm.c - IMA ADPCM encoder/decoder for PokeyMAX sample upload
 *
 * Encodes 8-bit signed PCM to 4-bit IMA ADPCM (4:1 compression).
 * PokeyMAX block RAM holds ~43KB; ADPCM extends effective capacity to ~168KB.
 *
 * 6502/cc65 optimisations applied vs original:
 *
 *  1. Encoder: predictor clamp replaces the XOR-0x8000 biased-unsigned trick.
 *     The XOR pair cost 4 extra 16-bit operations per sample; a sign-overflow
 *     check after each signed add/sub is cheaper on 6502.
 *
 *  2. Encoder: step_half / step_qtr precomputed once per nibble instead of
 *     shifting the working copy of 'step' twice in sequence.  Saves two
 *     16-bit right-shifts per sample.
 *
 *  3. Decoder: all arithmetic converted from int32_t to uint16_t / int16_t.
 *     Every 32-bit operation on 6502 compiles to ~4x the instructions of its
 *     16-bit equivalent.  diff fits in uint16_t (max = step * 1.875 < 65535
 *     because the step table tops out at 32767).  Overflow after the
 *     int16_t add/sub is detected by sign-change, same as the encoder.
 *
 *  4. Decoder inlined into adpcm_decode_block() as a macro.  On 6502 each
 *     JSR/RTS + parameter-passing round-trip is expensive; with sample_count
 *     potentially in the thousands this adds up fast.  The standalone
 *     adpcm_decode_nibble() wrapper is kept for the public API but just
 *     invokes the macro.
 */

#include <stdint.h>
#include "adpcm.h"

const uint16_t ima_step_table[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
       32767
};

const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

/* -------------------------------------------------------------------------
 * ADPCM_ENCODE_NIBBLE
 *
 * Changes from original:
 *   - pred_delta built from precomputed step_half / step_qtr (saves 2 shifts)
 *   - predictor updated via plain signed add/sub + sign-overflow clamp
 *     instead of the XOR-0x8000 / XOR-0x8000 round-trip (saves 4 16-bit ops)
 * ------------------------------------------------------------------------- */
#define ADPCM_ENCODE_NIBBLE(pcm_sample, predictor_var, step_index_var, out_nibble) \
    do {                                                                             \
        int16_t  pcm16;                                                              \
        uint16_t diff;                                                               \
        uint16_t step;                                                               \
        uint16_t step_half;                                                          \
        uint16_t step_qtr;                                                           \
        uint16_t pred_delta;                                                         \
        int16_t  new_pred;                                                           \
        uint8_t  nibble_local = 0;                                                   \
        int8_t   idx_local;                                                          \
                                                                                     \
        pcm16 = (int16_t)(pcm_sample) << 8;                                          \
        step  = ima_step_table[(step_index_var)];                                    \
        /* Precompute fractions once — saves two sequential >>1 on 'step' */         \
        step_half = step >> 1;                                                       \
        step_qtr  = step_half >> 1;                                                  \
                                                                                     \
        if (pcm16 < (predictor_var)) {                                               \
            nibble_local = 8;                                                        \
            diff = (uint16_t)((uint16_t)(predictor_var) - (uint16_t)pcm16);         \
        } else {                                                                     \
            diff = (uint16_t)((uint16_t)pcm16 - (uint16_t)(predictor_var));         \
        }                                                                            \
                                                                                     \
        pred_delta = (uint16_t)(step >> 3);                                          \
        if (diff >= step) {                                                          \
            nibble_local |= 4;                                                       \
            diff -= step;                                                            \
            pred_delta += step;                                                      \
        }                                                                            \
        if (diff >= step_half) {                                                     \
            nibble_local |= 2;                                                       \
            diff -= step_half;                                                       \
            pred_delta += step_half;                                                 \
        }                                                                            \
        if (diff >= step_qtr) {                                                      \
            nibble_local |= 1;                                                       \
            pred_delta += step_qtr;                                                  \
        }                                                                            \
                                                                                     \
        /* Clamp via sign-overflow check: cheaper than the XOR-0x8000 pair */       \
        if (nibble_local & 8u) {                                                     \
            new_pred = (int16_t)((predictor_var) - (int16_t)pred_delta);             \
            /* Underflow if result went positive (wrapped past -32768) */            \
            (predictor_var) = (new_pred > (predictor_var)) ? (int16_t)-32768         \
                                                           : new_pred;               \
        } else {                                                                     \
            new_pred = (int16_t)((predictor_var) + (int16_t)pred_delta);             \
            /* Overflow if result went negative (wrapped past +32767) */             \
            (predictor_var) = (new_pred < (predictor_var)) ? (int16_t)32767          \
                                                           : new_pred;               \
        }                                                                            \
                                                                                     \
        idx_local = (int8_t)(step_index_var) + ima_index_table[nibble_local & 0x0F];\
        if (idx_local < 0)  idx_local = 0;                                           \
        if (idx_local > 88) idx_local = 88;                                          \
        (step_index_var) = (uint8_t)idx_local;                                       \
                                                                                     \
        (out_nibble) = (uint8_t)(nibble_local & 0x0F);                               \
    } while (0)

/* -------------------------------------------------------------------------
 * ADPCM_DECODE_NIBBLE
 *
 * Inlineable macro version of the decoder.
 *
 * Changes from original adpcm_decode_nibble():
 *   - All arithmetic in uint16_t / int16_t instead of int32_t.
 *     diff max = step * (1 + 0.5 + 0.25 + 0.125) = step * 1.875.
 *     step table max = 32767, so diff max = 61438 — fits in uint16_t.
 *   - Predictor clamp uses sign-overflow check (same pattern as encoder)
 *     rather than comparing against ±32767/32768 as 32-bit constants.
 * ------------------------------------------------------------------------- */
#define ADPCM_DECODE_NIBBLE(nibble, predictor_var, step_index_var, out_pcm8)     \
    do {                                                                          \
        uint16_t step16   = ima_step_table[(step_index_var)];                    \
        uint16_t diff16   = step16 >> 3;                                         \
        int16_t  pred16   = (predictor_var);                                     \
        int16_t  new_pred16;                                                      \
        int8_t   idx16;                                                           \
                                                                                  \
        if ((nibble) & 4u) diff16 += step16;                                     \
        if ((nibble) & 2u) diff16 += (step16 >> 1);                              \
        if ((nibble) & 1u) diff16 += (step16 >> 2);                              \
                                                                                  \
        if ((nibble) & 8u) {                                                      \
            new_pred16 = (int16_t)(pred16 - (int16_t)diff16);                    \
            /* Underflow: subtraction wrapped past -32768 → result > original */ \
            (predictor_var) = (new_pred16 > pred16) ? (int16_t)-32768            \
                                                    : new_pred16;                 \
        } else {                                                                  \
            new_pred16 = (int16_t)(pred16 + (int16_t)diff16);                    \
            /* Overflow: addition wrapped past +32767 → result < original */     \
            (predictor_var) = (new_pred16 < pred16) ? (int16_t)32767             \
                                                    : new_pred16;                 \
        }                                                                         \
                                                                                  \
        idx16 = (int8_t)(step_index_var) + ima_index_table[(nibble) & 0x0Fu];   \
        if (idx16 < 0)  idx16 = 0;                                               \
        if (idx16 > 88) idx16 = 88;                                              \
        (step_index_var) = (uint8_t)idx16;                                       \
                                                                                  \
        (out_pcm8) = (int8_t)((predictor_var) >> 8);                             \
    } while (0)


/* ---- Public API wrappers ------------------------------------------------ */

uint8_t adpcm_encode_sample(int8_t pcm_sample, ADPCMState *state)
{
    uint8_t nibble;
    int16_t predictor  = state->predictor;
    uint8_t step_index = state->step_index;

    ADPCM_ENCODE_NIBBLE(pcm_sample, predictor, step_index, nibble);

    state->predictor  = predictor;
    state->step_index = step_index;

    return nibble;
}

#ifndef __CC65__
uint16_t adpcm_encode_block(const int8_t *src, uint16_t pcm_len,
                             uint8_t *dst, ADPCMState *state)
{
    uint16_t i;
    uint16_t out_bytes = 0;
    uint8_t  lo, hi;
    int16_t  predictor  = state->predictor;
    uint8_t  step_index = state->step_index;

    for (i = 0; i + 1 < pcm_len; i += 2) {
        ADPCM_ENCODE_NIBBLE(src[i],     predictor, step_index, hi);
        ADPCM_ENCODE_NIBBLE(src[i + 1], predictor, step_index, lo);
        dst[out_bytes++] = (uint8_t)((hi << 4) | lo);
    }
    if (pcm_len & 1u) {
        ADPCM_ENCODE_NIBBLE(src[pcm_len - 1], predictor, step_index, hi);
        dst[out_bytes++] = (uint8_t)(hi << 4);
    }

    state->predictor  = predictor;
    state->step_index = step_index;

    return out_bytes;
}
#endif

/* Standalone decode — kept for the public API; macro does the real work. */
int8_t adpcm_decode_nibble(uint8_t nibble, ADPCMState *state)
{
    int8_t  pcm8;
    int16_t predictor  = state->predictor;
    uint8_t step_index = state->step_index;

    ADPCM_DECODE_NIBBLE(nibble, predictor, step_index, pcm8);

    state->predictor  = predictor;
    state->step_index = step_index;

    return pcm8;
}

/*
 * adpcm_decode_block — hot path; macro inlined, no JSR per sample.
 *
 * State pulled into locals so cc65 can keep them in zero-page if available,
 * and to avoid pointer dereferences inside the tight loop.
 */
uint16_t adpcm_decode_block(const uint8_t *src, uint16_t sample_count,
                            int8_t *dst, ADPCMState *state)
{
    uint16_t i;
    int16_t  predictor  = state->predictor;
    uint8_t  step_index = state->step_index;

    for (i = 0; i < sample_count; i++) {
        uint8_t byte   = src[i >> 1];
        uint8_t nibble = (uint8_t)((i & 1u) ? (byte & 0x0Fu)
                                             : ((byte >> 4) & 0x0Fu));
        int8_t  pcm8;
        ADPCM_DECODE_NIBBLE(nibble, predictor, step_index, pcm8);
        dst[i] = pcm8;
    }

    state->predictor  = predictor;
    state->step_index = step_index;

    return sample_count;
}

#undef ADPCM_ENCODE_NIBBLE
#undef ADPCM_DECODE_NIBBLE
