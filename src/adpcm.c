/*
 * adpcm.c - IMA ADPCM encoder for PokeyMAX sample upload
 *
 * Encodes 8-bit signed PCM to 4-bit IMA ADPCM (4:1 compression).
 * PokeyMAX block RAM holds ~43KB; ADPCM extends effective capacity to ~168KB.
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

/*
 * Core nibble encoder used by both single-sample and block paths.
 * Uses mostly 16-bit math so cc65 generates tighter/faster code on 6502.
 */
#define ADPCM_ENCODE_NIBBLE(pcm_sample, predictor_var, step_index_var, out_nibble) \
    do {                                                                             \
        int16_t pcm16;                                                               \
        uint16_t diff;                                                               \
        uint16_t step;                                                               \
        uint16_t pred_delta;                                                         \
        uint16_t pred_biased;                                                        \
        uint8_t nibble_local = 0;                                                    \
        int8_t idx_local;                                                            \
                                                                                     \
        pcm16 = (int16_t)(pcm_sample) << 8;                                          \
        step = ima_step_table[(step_index_var)];                                     \
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
        step >>= 1;                                                                  \
        if (diff >= step) {                                                          \
            nibble_local |= 2;                                                       \
            diff -= step;                                                            \
            pred_delta += step;                                                      \
        }                                                                            \
        step >>= 1;                                                                  \
        if (diff >= step) {                                                          \
            nibble_local |= 1;                                                       \
            pred_delta += step;                                                      \
        }                                                                            \
                                                                                     \
        pred_biased = (uint16_t)(predictor_var) ^ 0x8000u;                           \
        if (nibble_local & 8u) {                                                     \
            if (pred_delta > pred_biased)                                            \
                pred_biased = 0u;                                                    \
            else                                                                     \
                pred_biased = (uint16_t)(pred_biased - pred_delta);                  \
        } else {                                                                     \
            if (pred_delta > (uint16_t)(0xFFFFu - pred_biased))                      \
                pred_biased = 0xFFFFu;                                               \
            else                                                                     \
                pred_biased = (uint16_t)(pred_biased + pred_delta);                  \
        }                                                                            \
        (predictor_var) = (int16_t)(pred_biased ^ 0x8000u);                          \
                                                                                     \
        idx_local = (int8_t)(step_index_var) + ima_index_table[nibble_local & 0x0F];\
        if (idx_local < 0)  idx_local = 0;                                           \
        if (idx_local > 88) idx_local = 88;                                          \
        (step_index_var) = (uint8_t)idx_local;                                       \
                                                                                     \
        (out_nibble) = (uint8_t)(nibble_local & 0x0F);                               \
    } while (0)

uint8_t adpcm_encode_sample(int8_t pcm_sample, ADPCMState *state)
{
    uint8_t nibble;
    int16_t predictor = state->predictor;
    uint8_t step_index = state->step_index;

    ADPCM_ENCODE_NIBBLE(pcm_sample, predictor, step_index, nibble);

    state->predictor = predictor;
    state->step_index = step_index;

    return nibble;
}

uint16_t adpcm_encode_block(const int8_t *src, uint16_t pcm_len,
                             uint8_t *dst, ADPCMState *state)
{
    uint16_t i;
    uint16_t out_bytes = 0;
    uint8_t lo, hi;
    int16_t predictor = state->predictor;
    uint8_t step_index = state->step_index;

    for (i = 0; i + 1 < pcm_len; i += 2) {
        ADPCM_ENCODE_NIBBLE(src[i], predictor, step_index, hi);
        ADPCM_ENCODE_NIBBLE(src[i + 1], predictor, step_index, lo);
        dst[out_bytes++] = (uint8_t)((hi << 4) | lo);
    }
    if (pcm_len & 1u) {
        ADPCM_ENCODE_NIBBLE(src[pcm_len - 1], predictor, step_index, hi);
        dst[out_bytes++] = (uint8_t)(hi << 4);
    }

    state->predictor = predictor;
    state->step_index = step_index;

    return out_bytes;
}

int8_t adpcm_decode_nibble(uint8_t nibble, ADPCMState *state)
{
    int32_t step = (int32_t)ima_step_table[state->step_index];
    int32_t diff = step >> 3;
    int32_t pred = state->predictor;
    int8_t idx;

    if (nibble & 4u) diff += step;
    if (nibble & 2u) diff += (step >> 1);
    if (nibble & 1u) diff += (step >> 2);

    if (nibble & 8u) pred -= diff;
    else             pred += diff;

    if (pred > 32767l) pred = 32767l;
    if (pred < -32768l) pred = -32768l;
    state->predictor = (int16_t)pred;

    idx = (int8_t)state->step_index + ima_index_table[nibble & 0x0Fu];
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    state->step_index = (uint8_t)idx;

    /* 16-bit predictor to signed 8-bit PCM domain */
    pred >>= 8;
    if (pred > 127) pred = 127;
    if (pred < -128) pred = -128;
    return (int8_t)pred;
}

uint16_t adpcm_decode_block(const uint8_t *src, uint16_t sample_count,
                            int8_t *dst, ADPCMState *state)
{
    uint16_t i;

    for (i = 0; i < sample_count; i++) {
        uint8_t byte = src[i >> 1];
        uint8_t nibble = (uint8_t)((i & 1u) ? (byte & 0x0Fu) : ((byte >> 4) & 0x0Fu));
        dst[i] = adpcm_decode_nibble(nibble, state);
    }

    return sample_count;
}

#undef ADPCM_ENCODE_NIBBLE
