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
 * Encode one 8-bit signed PCM sample to a 4-bit ADPCM nibble.
 * Use int32_t for predictor math so clamp happens before any narrowing.
 */
uint8_t adpcm_encode_sample(int8_t pcm_sample, ADPCMState *state)
{
    int32_t diff;
    int32_t step;
    int32_t pred_delta;
    int32_t next_pred;
    uint8_t nibble;
    int8_t  idx;
    int32_t pcm16 = (int32_t)((int16_t)pcm_sample << 8);

    step = (int32_t)ima_step_table[state->step_index];
    diff = pcm16 - (int32_t)state->predictor;

    nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    pred_delta = step >> 3;
    if (diff >= step)       { nibble |= 4; diff -= step; pred_delta += step; }
    step >>= 1;
    if (diff >= step)       { nibble |= 2; diff -= step; pred_delta += step; }
    step >>= 1;
    if (diff >= step)       { nibble |= 1;               pred_delta += step; }

    if (nibble & 8)
        next_pred = (int32_t)state->predictor - pred_delta;
    else
        next_pred = (int32_t)state->predictor + pred_delta;

    if (next_pred > 32767l)  next_pred = 32767l;
    if (next_pred < -32768l) next_pred = -32768l;
    state->predictor = (int16_t)next_pred;

    idx = (int8_t)state->step_index + ima_index_table[nibble & 0x0F];
    if (idx < 0)  idx = 0;
    if (idx > 88) idx = 88;
    state->step_index = (uint8_t)idx;

    return nibble & 0x0F;
}

uint16_t adpcm_encode_block(const int8_t *src, uint16_t pcm_len,
                             uint8_t *dst, ADPCMState *state)
{
    uint16_t i;
    uint16_t out_bytes = 0;
    uint8_t  lo, hi;

    for (i = 0; i + 1 < pcm_len; i += 2) {
        lo = adpcm_encode_sample(src[i],   state);
        hi = adpcm_encode_sample(src[i+1], state);
        dst[out_bytes++] = (uint8_t)((hi << 4) | lo);
    }
    if (pcm_len & 1) {
        lo = adpcm_encode_sample(src[pcm_len - 1], state);
        dst[out_bytes++] = lo;
    }
    return out_bytes;
}
