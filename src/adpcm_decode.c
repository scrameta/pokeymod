/*
 * adpcm_decode.c - IMA ADPCM decoder helpers for host/shim diagnostics.
 *
 * This file is intentionally separate from adpcm.c so Atari XEX builds that
 * only need ADPCM encoding/upload tables do not include decoder code.  The
 * Linux shim and host roundtrip tests link this file explicitly.
 */

#include <stdint.h>
#include "adpcm.h"

/* -------------------------------------------------------------------------
 * ADPCM_DECODE_NIBBLE
 *
 * Inlineable macro version of the decoder.
 *
 * Changes from original adpcm_decode_nibble():
 *   - All arithmetic in uint16_t / int16_t instead of int32_t.
 *     diff max = step * (1 + 0.5 + 0.25 + 0.125) = step * 1.875.
 *     step table max = 32767, so diff max = 61438 — fits in uint16_t.
 *   - Predictor clamp uses sign-overflow check rather than comparing against
 *     ±32767/32768 as 32-bit constants.
 * ------------------------------------------------------------------------- */
#define ADPCM_DECODE_NIBBLE(nibble, predictor_var, step_index_var, out_pcm8)     \
    do {                                                                          \
        uint16_t step16   = ima_step_table[(step_index_var)];                    \
        uint16_t diff16   = step16 >> 3;                                         \
        int16_t  pred16   = (predictor_var);                                     \
        int16_t  new_pred16;                                                     \
        int8_t   idx16;                                                          \
                                                                                  \
        if ((nibble) & 4u) diff16 += step16;                                     \
        if ((nibble) & 2u) diff16 += (step16 >> 1);                              \
        if ((nibble) & 1u) diff16 += (step16 >> 2);                              \
                                                                                  \
        if ((nibble) & 8u) {                                                      \
            new_pred16 = (int16_t)(pred16 - (int16_t)diff16);                    \
            (predictor_var) = (new_pred16 > pred16) ? (int16_t)-32768            \
                                                    : new_pred16;                \
        } else {                                                                  \
            new_pred16 = (int16_t)(pred16 + (int16_t)diff16);                    \
            (predictor_var) = (new_pred16 < pred16) ? (int16_t)32767             \
                                                    : new_pred16;                \
        }                                                                         \
                                                                                  \
        idx16 = (int8_t)(step_index_var) + ima_index_table[(nibble) & 0x0Fu];    \
        if (idx16 < 0)  idx16 = 0;                                               \
        if (idx16 > 88) idx16 = 88;                                              \
        (step_index_var) = (uint8_t)idx16;                                       \
                                                                                  \
        (out_pcm8) = (int8_t)((predictor_var) >> 8);                             \
    } while (0)

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

#undef ADPCM_DECODE_NIBBLE
