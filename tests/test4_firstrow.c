/*
 * test4_firstrow.c - Load MOD, upload samples, play row 0 once
 *
 * Tests the full pipeline without VBI:
 *   - File open + header parse
 *   - Sample upload to PokeyMAX block RAM
 *   - Seek to pattern data and read row 0 of pattern 0
 *   - Decode 4 notes and trigger the corresponding samples
 *   - No VBI, no looping, just a one-shot trigger
 *
 * If you hear the first row's notes play once and stop, everything
 * up to VBI integration is working.
 *
 * Build:
 *   cl65 -t atari -O -o test4.xex \
 *        test4_firstrow.c pokeymax_hw.c adpcm.c tables.c -I include
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <conio.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "adpcm.h"

#define MOD_MAX_SAMPLES   31
#define MOD_ROWS_PER_PAT  64
#define MOD_CHANNELS      4
#define SECTOR_SIZE       256
#define POKEYMAX_RAM_SIZE 43008U
#define POKEYMAX_ALLOC_FAIL 0xFFFFU

/* Amiga period table C-1..B-3 */
static const uint16_t periods[36] = {
    856,808,762,720,678,640,604,570,538,508,480,453,
    428,404,381,360,339,320,302,285,269,254,240,226,
    214,202,190,180,170,160,151,143,135,127,120,113
};

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

typedef struct {
    uint16_t length;
    uint8_t  finetune;
    uint8_t  volume;
    uint16_t loop_start;
    uint16_t loop_len;
    uint8_t  has_loop;
    uint16_t pokeymax_addr;
    uint16_t pokeymax_len;
    uint8_t  is_adpcm;
    uint8_t  is_8bit;
} SampleInfo;

static SampleInfo samples[32];
static uint8_t    order_table[128];
static uint16_t   ram_ptr = 0;
static uint8_t    sector_buf[SECTOR_SIZE];
static uint8_t    adpcm_out[SECTOR_SIZE / 2];

static uint16_t alloc_ram(uint16_t bytes)
{
    uint16_t addr;
    if ((uint32_t)ram_ptr + bytes > POKEYMAX_RAM_SIZE) return POKEYMAX_ALLOC_FAIL;
    addr = ram_ptr;
    ram_ptr += bytes;
    return addr;
}

static uint16_t period_to_hw(uint16_t p)
{
    uint32_t hw = (uint32_t)p * 2UL;  /* PokeyMAX clock=2*PHI2, same ratio as Paula */
    if (hw == 0) return 1;
    if (hw > 0xFFFF) return 0xFFFF;
    return (uint16_t)hw;
}

int main(void)
{
    FILE    *f;
    uint8_t  hdr[30];
    uint8_t  title[21];
    uint8_t  two[2];
    uint8_t  magic[4];
    uint8_t  row_buf[MOD_CHANNELS * 4];
    uint8_t  i, ch;
    uint8_t  song_length, num_patterns;
    uint32_t total_sample_bytes;
    uint8_t  use_adpcm;
    uint32_t pattern_offset;
    uint32_t sample_offset;
    const char *filename = "D1:MOD.DAT";

    clrscr();
    printf("Test 4: First Row\n");
    printf("-----------------\n");

    /* --- Detect PokeyMAX --- */
    printf("Detect...");
    if (PEEK(REG_CFGID) != 1) { printf("FAIL\n"); goto done; }
    POKE(REG_CFGUNLOCK, 0x3F);
    { uint8_t cap = PEEK(REG_CFG_CAP);
      POKE(REG_CFGUNLOCK, 0x00);
      if (!(cap & CAP_SAMPLE)) { printf("No sample player\n"); goto done; }
    }
    printf("OK\n");

    pokeymax_init();

    /* --- Open file --- */
    printf("Open %s...", filename);
    f = fopen(filename, "rb");
    if (!f) { printf("FAIL\n"); goto done; }
    printf("OK\n");

    /* --- Title --- */
    if (fread(title, 1, 20, f) != 20) { printf("title FAIL\n"); goto done; }
    title[20] = 0;
    printf("Title: [%s]\n", title);

    /* --- 31 sample headers --- */
    total_sample_bytes = 0;
    memset(samples, 0, sizeof(samples));
    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &samples[i];
        if (fread(hdr, 1, 30, f) != 30) { printf("hdr FAIL\n"); goto done; }
        si->length     = read_be16(hdr + 22) * 2;
        si->finetune   = hdr[24] & 0x0F;
        si->volume     = hdr[25] > 64 ? 64 : hdr[25];
        si->loop_start = read_be16(hdr + 26) * 2;
        si->loop_len   = read_be16(hdr + 28) * 2;
        si->has_loop   = (si->loop_len > 2) ? 1 : 0;
        if (si->length > 0) total_sample_bytes += si->length;
    }
    printf("Total sample bytes: %lu\n", total_sample_bytes);

    /* --- Song length + order table --- */
    if (fread(two, 1, 2, f) != 2) { printf("songlen FAIL\n"); goto done; }
    song_length = two[0];
    if (fread(order_table, 1, 128, f) != 128) { printf("orders FAIL\n"); goto done; }
    if (fread(magic, 1, 4, f) != 4) { printf("magic FAIL\n"); goto done; }

    num_patterns = 0;
    for (i = 0; i < song_length; i++)
        if (order_table[i] > num_patterns) num_patterns = order_table[i];
    num_patterns++;
    printf("Orders:%d Patterns:%d Magic:[%.4s]\n", song_length, num_patterns, magic);

    /* --- Pattern data offset --- */
    pattern_offset = (uint32_t)ftell(f);
    printf("Pattern data @ %lu\n", pattern_offset);

    /* --- Seek to sample data --- */
    sample_offset = pattern_offset
                  + (uint32_t)num_patterns * (uint32_t)(MOD_ROWS_PER_PAT * MOD_CHANNELS * 4);
    printf("Sample data @ %lu\n", sample_offset);
    if (fseek(f, (long)sample_offset, SEEK_SET) != 0) {
        printf("fseek to samples FAIL\n"); goto done;
    }
    printf("fseek OK\n");

    /* --- Upload samples --- */
    use_adpcm = 1; 
    //use_adpcm = (total_sample_bytes > (uint32_t)POKEYMAX_RAM_SIZE) ? 1 : 0;
    printf("Uploading samples (adpcm=%d)...\n", use_adpcm);

    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &samples[i];
        uint16_t    ram_addr, ram_needed;
        uint16_t    remaining, chunk, written, out_len;
        ADPCMState  adpcm_state;

        if (si->length == 0) continue;

        if (use_adpcm && si->length > 512 && !si->has_loop) {
            ram_needed = (si->length + 1) / 2;
            si->is_adpcm = 1; si->is_8bit = 0;
        } else {
            ram_needed = si->length;
            si->is_adpcm = 0; si->is_8bit = 1;
        }

        ram_addr = alloc_ram(ram_needed);
        if (ram_addr == POKEYMAX_ALLOC_FAIL) {
            printf("  S%d: RAM full, skip\n", i);
            fseek(f, (long)si->length, SEEK_CUR);
            si->length = 0;
            continue;
        }

        si->pokeymax_addr = ram_addr;
        si->pokeymax_len  = ram_needed;
        printf("  S%d: %u bytes->%u @ %u\n", i, si->length, ram_needed, ram_addr);

        adpcm_state.predictor = 0; adpcm_state.step_index = 0;
        remaining = si->length; written = 0;
        while (remaining > 0) {
            chunk = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            if (fread(sector_buf, 1, chunk, f) != chunk) {
                printf("  S%d read FAIL\n", i); goto done;
            }
            if (si->is_adpcm) {
                out_len = adpcm_encode_block((const int8_t*)sector_buf, chunk,
                                             adpcm_out, &adpcm_state);
                /* ADPCM data is already encoded nibbles; write raw bytes. */
                pokeymax_write_ram_raw(ram_addr + written, adpcm_out, out_len);
                written += out_len;
            } else {
                pokeymax_write_ram(ram_addr + written, sector_buf, chunk);
                written += chunk;
            }
            remaining -= chunk;
        }
    }
    printf("RAM used: %u/%u\n", ram_ptr, POKEYMAX_RAM_SIZE);

    /* --- Read row 0 of pattern 0 (first order) --- */
    {
        uint8_t pat0 = order_table[0];
        uint32_t row0_offset = pattern_offset
                             + (uint32_t)pat0 * (uint32_t)(MOD_ROWS_PER_PAT * MOD_CHANNELS * 4);
        printf("Row 0 of pattern %d @ %lu\n", pat0, row0_offset);
        if (fseek(f, (long)row0_offset, SEEK_SET) != 0) {
            printf("fseek row0 FAIL\n"); goto done;
        }
        if (fread(row_buf, 1, MOD_CHANNELS * 4, f) != MOD_CHANNELS * 4) {
            printf("read row0 FAIL\n"); goto done;
        }
    }

    /* --- Decode and trigger each channel --- */
    printf("Triggering row 0:\n");
    /* No IRQ handler - leave IRQ disabled, DMA works without it */

    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        uint8_t  b0 = row_buf[ch*4+0];
        uint8_t  b1 = row_buf[ch*4+1];
        uint8_t  b2 = row_buf[ch*4+2];
        uint8_t  b3 = row_buf[ch*4+3];
        uint8_t  snum   = (b0 & 0xF0) | ((b2 >> 4) & 0x0F);
        uint16_t period = ((uint16_t)(b0 & 0x0F) << 8) | b1;
        uint8_t  effect = b2 & 0x0F;
        uint8_t  param  = b3;
        uint8_t  hw     = ch + 1;

        printf("  Ch%d: snum=%d period=%u fx=%X p=%02X\n",
               hw, snum, period, effect, param);

        if (snum == 0 || snum > MOD_MAX_SAMPLES) continue;
        if (samples[snum].length == 0) continue;
        if (period == 0) continue;

        pokeymax_channel_setup(hw,
                               samples[snum].pokeymax_addr,
                               samples[snum].length,
                               period_to_hw(period),
                               samples[snum].volume > 63 ? 63 : samples[snum].volume,
                               samples[snum].is_8bit,
                               samples[snum].is_adpcm);
        printf("  Ch%d: triggered", hw);
    }

    /* Disable IRQ - no handler installed, would cause IRQ storm */
    POKE(REG_IRQEN, 0x00);
    fclose(f);
    POKE(0xD01A, 0x28);  /* orange border = we're alive */
    printf("\nPlaying row 0. Press key to stop.\n");

    POKE(0x02FC, 255);
    while (PEEK(0x02FC) == 255) ;
    POKE(0x02FC, 255);

    POKE(REG_DMA,   0x00);
    POKE(REG_IRQEN, 0x00);
    POKE(0xD01A, 0x00);
    printf("Stopped.\n");

done:
    printf("Press key to exit\n");
    POKE(0x02FC, 255);
    while (PEEK(0x02FC) == 255) ;
    return 0;
}
