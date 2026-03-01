/*
 * adpcm_roundtrip_linux.c - host-side ADPCM quality check
 *
 * Builds with gcc/clang on Linux and validates that encoding+decoding
 * preserves signal shape within a bounded lossy error.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_COUNT 4096

static int8_t clamp_s8(int32_t x)
{
    if (x > 127) return 127;
    if (x < -128) return -128;
    return (int8_t)x;
}

int main(void)
{
    int8_t input[SAMPLE_COUNT];
    int8_t output[SAMPLE_COUNT];
    uint8_t encoded[(SAMPLE_COUNT + 1) / 2];
    ADPCMState enc = {0, 0};
    ADPCMState dec = {0, 0};
    uint16_t encoded_len;
    int i;
    double mse = 0.0;
    double rmse;
    int peak = 0;

    for (i = 0; i < SAMPLE_COUNT; i++) {
        double t = (double)i / (double)SAMPLE_COUNT;
        double s1 = sin(2.0 * M_PI * 5.0 * t);
        double s2 = 0.35 * sin(2.0 * M_PI * 41.0 * t);
        double ramp = ((double)((i % 256) - 128)) / 128.0;
        int32_t v = (int32_t)((s1 + s2 + (0.2 * ramp)) * 90.0);
        if ((i % 127) == 0) v += 20; /* transient-ish content */
        input[i] = clamp_s8(v);
    }

    encoded_len = adpcm_encode_block(input, SAMPLE_COUNT, encoded, &enc);
    if (encoded_len != (SAMPLE_COUNT + 1) / 2) {
        fprintf(stderr, "encoded size mismatch: got %u expected %u\n",
                encoded_len, (unsigned)((SAMPLE_COUNT + 1) / 2));
        return 1;
    }

    /* PokeyMAX ADPCM consumes high nibble first (RTL top.vhdl). */
    adpcm_decode_block(encoded, SAMPLE_COUNT, output, &dec);

    for (i = 0; i < SAMPLE_COUNT; i++) {
        int err = (int)input[i] - (int)output[i];
        int aerr = err < 0 ? -err : err;
        mse += (double)(err * err);
        if (aerr > peak) peak = aerr;
    }

    mse /= (double)SAMPLE_COUNT;
    rmse = sqrt(mse);

    printf("ADPCM roundtrip: samples=%d rmse=%.2f peak=%d\n", SAMPLE_COUNT, rmse, peak);

    if (rmse > 14.0 || peak > 70) {
        fprintf(stderr, "quality gate failed\n");
        return 2;
    }

    return 0;
}
