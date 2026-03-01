/*
 * adpcm.h / adpcm.c (single-header style for cc65)
 *
 * IMA ADPCM codec helpers for PokeyMAX data format.
 *
 * Encoder: converts 8-bit signed PCM → 4-bit IMA ADPCM.
 * Decoder: converts 4-bit IMA ADPCM nibble → 8-bit signed PCM.
 * Compatible with sox "-e ima-adpcm" output as used by PokeyMAX.
 *
 * Output format: nibbles packed high-nibble-first per byte for PokeyMAX
 * (first sample in high nibble, second sample in low nibble).
 * 2 samples per byte → 4:1 compression on 8-bit input.
 */

#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

/* IMA ADPCM step table (89 entries) */
extern const uint16_t ima_step_table[89];

/* IMA ADPCM index adjustment table */
extern const int8_t ima_index_table[16];

/* Encoder state */
typedef struct {
    int16_t  predictor;   /* running predictor */
    uint8_t  step_index;  /* index into step_table */
} ADPCMState;

/*
 * adpcm_encode_sample()
 * Encode one 8-bit signed PCM sample → 4-bit ADPCM nibble (0-15).
 * Updates state in place.
 */
uint8_t adpcm_encode_sample(int8_t pcm_sample, ADPCMState *state);

/*
 * adpcm_encode_block()
 * Encode 'pcm_len' bytes of 8-bit signed PCM from 'src' into IMA ADPCM.
 * Output written to 'dst'. Returns number of bytes written.
 * dst must be at least (pcm_len + 1) / 2 bytes.
 * Nibbles packed: high nibble = first sample, low nibble = second sample.
 */
uint16_t adpcm_encode_block(const int8_t *src, uint16_t pcm_len,
                             uint8_t *dst, ADPCMState *state);

/*
 * adpcm_decode_nibble()
 * Decode one 4-bit IMA ADPCM nibble (0-15) into signed 8-bit PCM.
 * Updates state in place.
 */
int8_t adpcm_decode_nibble(uint8_t nibble, ADPCMState *state);

/*
 * adpcm_decode_block()
 * Decode 'sample_count' samples from packed IMA ADPCM in 'src' to 8-bit PCM.
 * Nibble order matches PokeyMAX RAM format: high nibble then low nibble.
 * Returns number of decoded samples written to 'dst' (equals sample_count).
 */
uint16_t adpcm_decode_block(const uint8_t *src, uint16_t sample_count,
                            int8_t *dst, ADPCMState *state);

#endif /* ADPCM_H */
