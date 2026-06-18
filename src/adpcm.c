/*
 * adpcm.c - IMA ADPCM encoder and tables for PokeyMAX sample upload
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
 * Decoder helpers live in adpcm_decode.c so Atari XEX builds that only need
 * encode/upload support do not pull decoder code into memory.
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

#undef ADPCM_ENCODE_NIBBLE
