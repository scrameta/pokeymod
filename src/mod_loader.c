/*
 * mod_loader.c - MOD loader with double-buffered pattern cache
 *
 * Pattern RAM strategy (no VBI disk I/O):
 *
 *   Two 1KB pattern buffers. 'current' is what the VBI reads (memcpy only,
 *   no disk). 'next' is filled by the main loop between VBIs.
 *
 *   When the player advances to a new pattern, mod_pattern_advance() swaps
 *   the pointers and sets mod_need_prefetch=1. The main loop then calls
 *   mod_prefetch_next_pattern() which does the actual fseek+fread.
 *
 *   The 1050 disk drive takes ~100ms to seek + read 1KB. At 125BPM/speed6
 *   a pattern lasts ~7.5 seconds, so there is ample time.
 *
 *   Total pattern RAM: 2 x 1024 = 2KB. Works on stock 64KB 800XL.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mod_format.h"
#include "modplayer.h"
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "adpcm.h"

#define PAT_BYTES   (MOD_ROWS_PER_PAT * MOD_CHANNELS * 4)   /* 1024 */

static uint8_t pat_buf_a[PAT_BYTES];
static uint8_t pat_buf_b[PAT_BYTES];

static uint8_t *pat_current    = pat_buf_a;
static uint8_t *pat_next       = pat_buf_b;
static uint8_t  pat_current_num = 0xFF;
static uint8_t  pat_next_num    = 0xFF;

uint8_t mod_need_prefetch = 0;   /* main loop polls this */

static FILE    *mod_file            = NULL;
static uint32_t pattern_data_offset = 0;

#define SECTOR_SIZE 256
static uint8_t sector_buf[SECTOR_SIZE];
static uint8_t adpcm_out[SECTOR_SIZE / 2];

uint8_t mod_row_buf[MOD_CHANNELS * 4];

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

void mod_file_close(void)
{
    if (mod_file) { fclose(mod_file); mod_file = NULL; }
}

/* -------------------------------------------------------
 * mod_read_row() - called from VBI, NO disk I/O
 * ------------------------------------------------------- */
uint8_t mod_read_row(uint8_t pattern, uint8_t row)
{
    uint16_t offset = (uint16_t)row * (MOD_CHANNELS * 4u);

    if (pattern == pat_current_num) {
        memcpy(mod_row_buf, pat_current + offset, MOD_CHANNELS * 4);
        return 0;
    }
    if (pattern == pat_next_num) {
        memcpy(mod_row_buf, pat_next + offset, MOD_CHANNELS * 4);
        return 0;
    }
    return 1;   /* not buffered - prefetch didn't complete in time */
}

/* -------------------------------------------------------
 * mod_pattern_advance() - called from VBI when order advances
 * Flips buffers, requests next prefetch. NO disk I/O.
 * ------------------------------------------------------- */
void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next)
{
    uint8_t *tmp;

    if (new_current == pat_next_num) {
        /* Swap: next becomes current */
        tmp         = pat_current;
        pat_current = pat_next;
        pat_next    = tmp;
        pat_current_num = new_current;
        pat_next_num    = 0xFF;
    }

    if (prefetch_next != 0xFF && prefetch_next != pat_current_num) {
        pat_next_num      = prefetch_next;
        mod_need_prefetch = 1;
    }
}

/* -------------------------------------------------------
 * mod_prefetch_next_pattern() - called from MAIN LOOP only
 * Reads pat_next_num from disk into pat_next buffer.
 * ------------------------------------------------------- */
uint8_t mod_prefetch_next_pattern(void)
{
    uint32_t offset;

    if (!mod_need_prefetch)   return 0;
    if (!mod_file)            return 1;
    if (pat_next_num == 0xFF) { mod_need_prefetch = 0; return 0; }

    offset = pattern_data_offset
           + (uint32_t)pat_next_num * (uint32_t)PAT_BYTES;

    if (fseek(mod_file, (long)offset, SEEK_SET) != 0) return 1;
    if (fread(pat_next, 1, PAT_BYTES, mod_file) != PAT_BYTES) return 1;

    mod_need_prefetch = 0;
    return 0;
}

/* -------------------------------------------------------
 * mod_load()
 * ------------------------------------------------------- */
uint8_t mod_load(const char *filename)
{
    uint8_t  i;
    uint8_t  hdr_buf[30];
    uint8_t  title[20];
    uint8_t  two[2];
    uint8_t  magic[4];
    uint32_t total_sample_bytes;
    uint8_t  total_samples_to_load;
    uint8_t  loaded_samples;
    uint32_t loaded_source_bytes;
    uint32_t loaded_stored_bytes;
    uint8_t  use_adpcm_global;
    uint32_t sample_data_offset;

    memset(&mod, 0, sizeof(mod));
    pat_current_num   = 0xFF;
    pat_next_num      = 0xFF;
    mod_need_prefetch = 0;

    if (mod_file) { fclose(mod_file); mod_file = NULL; }
    mod_file = fopen(filename, "rb");
    if (!mod_file) return 1;

    if (fread(title, 1, 20, mod_file) != 20) return 1;
    (void)title;

    total_sample_bytes = 0;
    total_samples_to_load = 0;
    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &mod.samples[i];
        uint8_t j;
        if (fread(hdr_buf, 1, 30, mod_file) != 30) return 1;
        for (j = 0; j < MOD_SAMPLE_NAME_LEN; j++) {
            uint8_t c = hdr_buf[j];
            if (c == 0u) break;
            if (c < 32u || c > 126u) c = '.';
            si->name[j] = (char)c;
        }
        while (j < MOD_SAMPLE_NAME_LEN) si->name[j++] = 0;
        si->name[MOD_SAMPLE_NAME_LEN] = 0;
        si->length     = read_be16(hdr_buf + 22) * 2u;
        si->finetune   = (int8_t)(hdr_buf[24] & 0x0Fu);
        if (si->finetune > 7) si->finetune -= 16;
        si->volume     = (hdr_buf[25] > 64u) ? 64u : hdr_buf[25];
        si->loop_start = read_be16(hdr_buf + 26) * 2u;
        si->loop_len   = read_be16(hdr_buf + 28) * 2u;
        si->has_loop   = (si->loop_len > 2u) ? 1u : 0u;
        if (si->has_loop && (si->loop_start + si->loop_len) > si->length)
            si->loop_len = si->length - si->loop_start;
        if (si->length > 0u) {
            total_sample_bytes += si->length;
            total_samples_to_load++;
        }
    }

    if (fread(two, 1, 2, mod_file) != 2) return 1;
    mod.song_length = two[0];

    if (fread(mod.order_table, 1, MOD_MAX_PATTERNS, mod_file) != MOD_MAX_PATTERNS) return 1;
    if (fread(magic, 1, 4, mod_file) != 4) return 1;
    (void)magic;

    mod.num_patterns = 0;
    for (i = 0; i < mod.song_length; i++)
        if (mod.order_table[i] > mod.num_patterns)
            mod.num_patterns = mod.order_table[i];
    mod.num_patterns++;

    pattern_data_offset = (uint32_t)ftell(mod_file);

    use_adpcm_global = (total_sample_bytes > (uint32_t)POKEYMAX_RAM_SIZE) ? 1u : 0u;

    sample_data_offset = pattern_data_offset
                       + (uint32_t)mod.num_patterns * (uint32_t)PAT_BYTES;
    if (fseek(mod_file, (long)sample_data_offset, SEEK_SET) != 0) return 1;

    pokeymax_init();
    loaded_samples      = 0;
    loaded_source_bytes = 0;
    loaded_stored_bytes = 0;

    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &mod.samples[i];
        uint16_t    ram_addr, ram_needed;
        uint16_t    remaining, chunk, written, out_len;
        ADPCMState  adpcm_state;

        if (si->length == 0u) continue;

        /*
         * ADPCM + sample-end loop retrigger causes decoder state reset in hardware
         * (DMA toggle -> syncreset), which badly distorts looped instruments.
         * Keep looped samples in 8-bit PCM; ADPCM only for non-looping samples.
         */
        if (use_adpcm_global && si->length > 512u && !si->has_loop) {
            ram_needed = (si->length + 1u) / 2u;
            si->is_adpcm = 1; si->is_8bit = 0;
        } else {
            ram_needed = si->length;
            si->is_adpcm = 0; si->is_8bit = 1;
        }

        ram_addr = pokeymax_alloc(ram_needed);
        if (ram_addr == POKEYMAX_ALLOC_FAIL) {
            printf("\rLoading \"%s\" (%u bytes, %s) | sample %u/%u | source %lu/%lu bytes | stored %lu bytes [SKIPPED: no RAM]\n",
                   si->name[0] ? si->name : "(unnamed)",
                   (unsigned)si->length,
                   si->is_adpcm ? "ADPCM" : "PCM",
                   (unsigned)(loaded_samples + 1u),
                   (unsigned)total_samples_to_load,
                   (unsigned long)loaded_source_bytes,
                   (unsigned long)total_sample_bytes,
                   (unsigned long)loaded_stored_bytes);
            fflush(stdout);
            fseek(mod_file, (long)si->length, SEEK_CUR);
            si->length = 0;
            continue;
        }

        si->pokeymax_addr = ram_addr;
        si->pokeymax_len  = ram_needed;

        adpcm_state.predictor  = 0;
        adpcm_state.step_index = 0;
        remaining = si->length;
        written   = 0;

        while (remaining > 0u) {
            chunk = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            if (fread(sector_buf, 1, chunk, mod_file) != chunk) return 1;
            if (si->is_adpcm) {
                out_len = adpcm_encode_block((const int8_t*)sector_buf, chunk,
                                             adpcm_out, &adpcm_state);
                pokeymax_write_ram(ram_addr + written, adpcm_out, out_len);
                written += out_len;
            } else {
                pokeymax_write_ram(ram_addr + written, sector_buf, chunk);
                written += chunk;
            }
            remaining -= chunk;

            printf("\rLoading \"%s\" (%u bytes, %s) | sample %u/%u | source %lu/%lu bytes | stored %lu bytes",
                   si->name[0] ? si->name : "(unnamed)",
                   (unsigned)si->length,
                   si->is_adpcm ? "ADPCM" : "PCM",
                   (unsigned)(loaded_samples + 1u),
                   (unsigned)total_samples_to_load,
                   (unsigned long)(loaded_source_bytes + (si->length - remaining)),
                   (unsigned long)total_sample_bytes,
                   (unsigned long)(loaded_stored_bytes + written));
            fflush(stdout);
        }

        loaded_samples++;
        loaded_source_bytes += si->length;
        loaded_stored_bytes += si->pokeymax_len;
        printf("\n");
        fflush(stdout);
    }

    /* Pre-load pattern for order 0 into current buffer */
    {
        uint32_t off = pattern_data_offset
                     + (uint32_t)mod.order_table[0] * (uint32_t)PAT_BYTES;
        if (fseek(mod_file, (long)off, SEEK_SET) != 0) return 1;
        if (fread(pat_current, 1, PAT_BYTES, mod_file) != PAT_BYTES) return 1;
        pat_current_num = mod.order_table[0];
    }

    /* Pre-load pattern for order 1 into next buffer */
    if (mod.song_length > 1u) {
        uint32_t off = pattern_data_offset
                     + (uint32_t)mod.order_table[1] * (uint32_t)PAT_BYTES;
        if (fseek(mod_file, (long)off, SEEK_SET) != 0) return 1;
        if (fread(pat_next, 1, PAT_BYTES, mod_file) != PAT_BYTES) return 1;
        pat_next_num = mod.order_table[1];
    }

    mod.order_pos     = 0;
    mod.row           = 0;
    mod.tick          = 0;
    mod.speed         = DEFAULT_SPEED;
    mod.bpm           = DEFAULT_BPM;
    mod.loop_song     = 1;
    mod.playing       = 0;
    mod.pattern_break = 0;
    mod.do_jump       = 0;
    mod.pattern_delay = 0;

    return 0;
}
