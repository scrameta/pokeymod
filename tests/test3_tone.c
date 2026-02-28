/*
 * test3_tone.c - Write a sine tone to PokeyMAX block RAM and play it
 *
 * Build: cl65 -t atari -O --include-dir include -o test3.xex \
 *             tests/test3_tone.c src/pokeymax_hw.c
 */

#include <stdio.h>
#include <stdint.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include <conio.h>

/*
 * 256-byte sine, 8-bit unsigned - will be written in place to 2s complement
 * pokeymax_write_ram() 
 */
static unsigned char sine256[256] = {
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

/*
 * Period for ~440Hz A-4: 
 * (L*P)/(PHI2*2) = 440  
 * P = PHI2*2/(440*L) = 3600000/440*256 ~= 32
 */
#define SAMPLE_PERIOD  32  
#define TONE_VOL     0x3F

int main(void)
{
    unsigned char cap;
    uint16_t i;

    clrscr();
    printf("Test 3: PokeyMAX Tone\n");
    printf("---------------------\n");

    if (PEEK(REG_CFGID) != 1) { printf("No PokeyMAX\n"); goto done; }
    POKE(REG_CFGUNLOCK, 0x3F);
    cap = PEEK(REG_CFG_CAP);
    POKE(REG_CFGUNLOCK, 0x00);
    printf("cap=$%02X\n", (unsigned)cap);
    if (!(cap & CAP_SAMPLE)) { printf("No sample player\n"); goto done; }

    pokeymax_init();
    printf("Init OK\n");

    printf("Upload sine (256 bytes)...");
    for (i=0;i!=256;++i)
	    sine256[i] = sine256[i]^128u;
    pokeymax_write_ram(0u, sine256, 256u);
    printf("OK\n");

    POKE(REG_CHANSEL, 1);
    POKE(REG_ADDRL,   0x00);
    POKE(REG_ADDRH,   0x00);
    POKE(REG_LENL,    0xFF);   /* len-1=255: plays 256 samples (doc: Length=L+H*256+1) */
    POKE(REG_LENH,    0x00);
    POKE(REG_PERL,    (unsigned char)(SAMPLE_PERIOD & 0xFF));
    POKE(REG_PERH,    (unsigned char)((SAMPLE_PERIOD >> 8) & 0x0F));
    POKE(REG_VOL,     TONE_VOL);

    POKE(REG_SAMCFG, 0xF0);    /* bits8=1111, adpcm=0000 */
    POKE(REG_IRQEN,  0x01);
    POKE(REG_DMA,    0x01);    /* 0->1 = trigger */

    printf("Playing ~440Hz. Border flickers = CPU alive.\n");
    printf("Press any key to stop.\n");

    POKE(0xD01A, 0x28);
    POKE(0x02FC, 0xFF);
    while (PEEK(0x02FC) == 0xFF)
        POKE(0xD01A, (unsigned char)(0x20 | (PEEK(RTCLOK) >> 2)));
    POKE(0x02FC, 0xFF);

    POKE(REG_DMA,   0x00);
    POKE(REG_IRQEN, 0x00);
    POKE(0xD01A,    0x00);
    printf("Stopped.\n");

done:
    printf("Press key to exit.\n");
    POKE(0x02FC, 0xFF);
    while (PEEK(0x02FC) == 0xFF) ;
    return 0;
}
