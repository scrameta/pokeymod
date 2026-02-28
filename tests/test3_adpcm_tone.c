/*
 * test3_adpcm_tone.c - Encode a sine wave to IMA ADPCM and play on PokeyMAX
 *
 * Purpose: isolate ADPCM decode path on real hardware without sample looping.
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "adpcm.h"

#define PCM_SAMPLES     1024u
#define ADPCM_BYTES    ((PCM_SAMPLES + 1u) / 2u)
#define SAMPLE_PERIOD    32u
#define TONE_VOL       0x3Fu

static const uint8_t sine256[256] = {
    128,131,134,137,140,143,146,149,152,155,158,161,164,167,170,173,
    176,179,182,184,187,190,192,195,198,200,203,205,207,210,212,214,
    216,218,220,222,224,226,228,229,231,232,234,235,237,238,239,240,
    241,242,243,244,244,245,246,246,247,247,247,248,248,248,248,248,
    248,248,248,248,248,247,247,247,246,246,245,244,244,243,242,241,
    240,239,238,237,235,234,232,231,229,228,226,224,222,220,218,216,
    214,212,210,207,205,203,200,198,195,192,190,187,184,182,179,176,
    173,170,167,164,161,158,155,152,149,146,143,140,137,134,131,128,
    124,121,118,115,112,109,106,103,100, 97, 94, 91, 88, 85, 82, 79,
     76, 73, 70, 68, 65, 62, 60, 57, 54, 52, 49, 47, 45, 42, 40, 38,
     36, 34, 32, 30, 28, 26, 24, 23, 21, 20, 18, 17, 15, 14, 13, 12,
     11, 10,  9,  8,  8,  7,  6,  6,  5,  5,  5,  4,  4,  4,  4,  4,
      4,  4,  4,  4,  4,  5,  5,  5,  6,  6,  7,  8,  8,  9, 10, 11,
     12, 13, 14, 15, 17, 18, 20, 21, 23, 24, 26, 28, 30, 32, 34, 36,
     38, 40, 42, 45, 47, 49, 52, 54, 57, 60, 62, 65, 68, 70, 73, 76,
     79, 82, 85, 88, 91, 94, 97,100,103,106,109,112,115,118,121,124
};

static int8_t pcm_buf[PCM_SAMPLES];
static uint8_t adpcm_buf[ADPCM_BYTES];

int main(void)
{
    uint8_t cap;
    uint16_t i;
    uint16_t out_len;
    ADPCMState st;

    clrscr();
    printf("Test 3 ADPCM: PokeyMAX sine (no loop)\n");
    printf("--------------------------------------\n");

    if (PEEK(REG_CFGID) != 1) { printf("No PokeyMAX\n"); goto done; }
    POKE(REG_CFGUNLOCK, 0x3F);
    cap = PEEK(REG_CFG_CAP);
    POKE(REG_CFGUNLOCK, 0x00);
    printf("cap=$%02X\n", (unsigned)cap);
    if (!(cap & CAP_SAMPLE)) { printf("No sample player\n"); goto done; }

    for (i = 0; i < PCM_SAMPLES; i++) {
        uint8_t u = sine256[i & 0xFFu];
        pcm_buf[i] = (int8_t)((int16_t)u - 128);
    }

    st.predictor = 0;
    st.step_index = 0;
    out_len = adpcm_encode_block(pcm_buf, PCM_SAMPLES, adpcm_buf, &st);
    printf("Encoded %u PCM -> %u ADPCM bytes\n", PCM_SAMPLES, out_len);

    pokeymax_init();
    pokeymax_write_ram(0u, adpcm_buf, out_len);

    POKE(REG_CHANSEL, 1);
    POKE(REG_ADDRL,   0x00);
    POKE(REG_ADDRH,   0x00);
    POKE(REG_LENL,    (uint8_t)((PCM_SAMPLES - 1u) & 0xFFu));
    POKE(REG_LENH,    (uint8_t)(((PCM_SAMPLES - 1u) >> 8) & 0xFFu));
    POKE(REG_PERL,    (uint8_t)(SAMPLE_PERIOD & 0xFFu));
    POKE(REG_PERH,    (uint8_t)((SAMPLE_PERIOD >> 8) & 0x0Fu));
    POKE(REG_VOL,     TONE_VOL);

    /* ch1 ADPCM on, ch1 bits8 off; other channels remain 8-bit PCM off */
    POKE(REG_SAMCFG, 0xE1);
    POKE(REG_IRQACT, 0x00);
    POKE(REG_IRQEN,  0x01);
    POKE(REG_DMA,    0x01);

    printf("Playing one-shot ADPCM sine; press key to stop early.\n");

    while (1) {
        if (PEEK(REG_IRQACT) & 0x01u) {
            printf("Playback finished (sample-end IRQ).\n");
            break;
        }
        if (kbhit()) {
            (void)cgetc();
            printf("Stopped by key.\n");
            break;
        }
    }

    POKE(REG_DMA,   0x00);
    POKE(REG_IRQEN, 0x00);
    POKE(REG_IRQACT,0x00);

done:
    printf("Press key to exit.\n");
    while (!kbhit()) ;
    (void)cgetc();
    return 0;
}
