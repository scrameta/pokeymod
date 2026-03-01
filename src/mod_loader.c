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
#include <stdlib.h>
#if defined(__CC65__)
#include <conio.h>
#include <atari.h>
#endif
#include "mod_format.h"
#include "modplayer.h"
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "adpcm.h"

#define PAT_BYTES   (MOD_ROWS_PER_PAT * MOD_CHANNELS * 4)   /* 1024 */
#define PATTERN_RAM_CACHE_THRESHOLD_BYTES (16u * 1024u)

static uint8_t pat_buf_a[PAT_BYTES];
static uint8_t pat_buf_b[PAT_BYTES];

static uint8_t *pat_current    = pat_buf_a;
static uint8_t *pat_next       = pat_buf_b;
static uint8_t  pat_current_num = 0xFF;
static uint8_t  pat_next_num    = 0xFF;

uint8_t mod_need_prefetch = 0;   /* main loop polls this */

static FILE    *mod_file            = NULL;
static uint32_t pattern_data_offset = 0;
static uint8_t *pattern_ram_cache = NULL;
static uint16_t pattern_ram_cache_size = 0;

/* Minimal pluggable fetch backend (Step A): default = disk fetch) */
typedef uint8_t (*PatternFetchFn)(uint8_t pattern_num, uint8_t *dst);
static uint8_t disk_fetch_pattern(uint8_t pattern_num, uint8_t *dst);
static uint8_t ram_fetch_pattern(uint8_t pattern_num, uint8_t *dst);
static PatternFetchFn s_fetch_pattern = disk_fetch_pattern;

/* Chunked prefetch state (256-byte slices to shorten SIO stalls) */
static uint8_t  prefetch_seek_done = 0;
static uint16_t prefetch_bytes_done = 0;

#define SECTOR_SIZE 256
static uint8_t sector_buf[SECTOR_SIZE];
static uint8_t adpcm_out[SECTOR_SIZE / 2];

uint8_t mod_row_buf[MOD_CHANNELS * 4];

typedef struct {
#if defined(__CC65__)
    uint8_t  base_row;
    uint16_t sample_index;
    uint16_t total_samples;
    uint16_t sample_len;
    uint32_t source_loaded;
    uint32_t source_total;
    uint32_t stored_loaded;
    uint8_t  is_adpcm;
    uint8_t  skipped;
    char     sample_name[23];
#endif
} LoadProgressUI;

#if defined(__CC65__)
static uint8_t pct_u8(uint32_t part, uint32_t total)
{
    if (total == 0u) return 100u;
    if (part >= total) return 100u;
    return (uint8_t)((part * 100u) / total);
}
#endif

static void load_progress_begin_default(void *ctx, uint32_t source_total)
{
    LoadProgressUI *ui = (LoadProgressUI*)ctx;
#if defined(__CC65__)
    ui->base_row      = wherey();
    ui->sample_index  = 0xFFFFu;
    ui->total_samples = 0xFFFFu;
    ui->sample_len    = 0xFFFFu;
    ui->source_loaded = 0xFFFFFFFFUL;
    ui->source_total  = 0xFFFFFFFFUL;
    ui->stored_loaded = 0xFFFFFFFFUL;
    ui->is_adpcm      = 0xFFu;
    ui->skipped       = 0xFFu;
    ui->sample_name[0]= 0;

    printf("Load 00/00   0%%\n");
    printf("Name: %-22.22s\n", "");
    printf("Fmt:----- Len:00000 St:-------\n");
    printf("Src:%8lu/%8lu\n", 0UL, (unsigned long)source_total);
    printf("Dst:00000000\n");
    ui->source_total = source_total;
    fflush(stdout);
#else
    (void)ui;
    printf("\r[ 0/ 0] %-22.22s  %-5s  len:%5u  src:%9lu/%9lu  dst:%9lu  %-7s   ",
           "",
           "",
           0u,
           0UL,
           (unsigned long)source_total,
           0UL,
           "");
    fflush(stdout);
#endif
}

static void load_progress_update_default(void *ctx,
                                         const SampleInfo *si,
                                         uint16_t sample_index,
                                         uint16_t total_samples,
                                         uint32_t source_loaded,
                                         uint32_t source_total,
                                         uint32_t stored_loaded,
                                         uint8_t skipped)
{
    LoadProgressUI *ui = (LoadProgressUI*)ctx;
#if defined(__CC65__)
    const char *name = si->name[0] ? si->name : "(unnamed)";
    const char *fmt  = si->is_adpcm ? "ADPCM" : "PCM";
    const char *st   = skipped ? "SKIPPED" : "OK";
    uint8_t pct      = pct_u8(source_loaded, source_total);

    if (sample_index != ui->sample_index || total_samples != ui->total_samples) {
        gotoxy(5, ui->base_row);
        printf("%2u/%2u", (unsigned)sample_index, (unsigned)total_samples);
        ui->sample_index  = sample_index;
        ui->total_samples = total_samples;
    }

    if (source_loaded != ui->source_loaded || source_total != ui->source_total) {
        gotoxy(11, ui->base_row);
        printf("%3u%%", (unsigned)pct);
    }

    if (strncmp(name, ui->sample_name, 22u) != 0) {
        gotoxy(6, ui->base_row + 1u);
        printf("%-22.22s", name);
        strncpy(ui->sample_name, name, 22u);
        ui->sample_name[22] = 0;
    }

    if (si->is_adpcm != ui->is_adpcm) {
        gotoxy(4, ui->base_row + 2u);
        printf("%-5s", fmt);
        ui->is_adpcm = si->is_adpcm;
    }

    if (si->length != ui->sample_len) {
        gotoxy(14, ui->base_row + 2u);
        printf("%5u", (unsigned)si->length);
        ui->sample_len = si->length;
    }

    if (skipped != ui->skipped) {
        gotoxy(23, ui->base_row + 2u);
        printf("%-7s", st);
        ui->skipped = skipped;
    }

    if (source_loaded != ui->source_loaded) {
        gotoxy(4, ui->base_row + 3u);
        printf("%8lu", (unsigned long)source_loaded);
        ui->source_loaded = source_loaded;
    }

    if (source_total != ui->source_total) {
        gotoxy(13, ui->base_row + 3u);
        printf("%8lu", (unsigned long)source_total);
        ui->source_total = source_total;
    }

    if (stored_loaded != ui->stored_loaded) {
        gotoxy(4, ui->base_row + 4u);
        printf("%8lu", (unsigned long)stored_loaded);
        ui->stored_loaded = stored_loaded;
    }

    fflush(stdout);
#else
    (void)ui;
    printf("\r[%2u/%2u] %-22.22s  %-5s  len:%5u  src:%9lu/%9lu  dst:%9lu  %-7s   ",
           (unsigned)sample_index,
           (unsigned)total_samples,
           si->name[0] ? si->name : "(unnamed)",
           si->is_adpcm ? "ADPCM" : "PCM",
           (unsigned)si->length,
           (unsigned long)source_loaded,
           (unsigned long)source_total,
           (unsigned long)stored_loaded,
           skipped ? "SKIPPED" : "OK");
    fflush(stdout);
#endif
}

static void load_progress_end_default(void *ctx)
{
    LoadProgressUI *ui = (LoadProgressUI*)ctx;
#if defined(__CC65__)
    gotoxy(0, ui->base_row + 5u);
#else
    (void)ui;
    printf("\n");
#endif
    fflush(stdout);
}

static LoadProgressUI s_default_progress_ctx;
const ModLoadProgressPlugin mod_default_load_progress_plugin = {
    load_progress_begin_default,
    load_progress_update_default,
    load_progress_end_default,
    &s_default_progress_ctx
};

static const ModLoadProgressPlugin *s_progress_plugin = &mod_default_load_progress_plugin;

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

void mod_file_close(void)
{
    if (mod_file) { fclose(mod_file); mod_file = NULL; }
    if (pattern_ram_cache) {
        free(pattern_ram_cache);
        pattern_ram_cache = NULL;
        pattern_ram_cache_size = 0;
    }
}

void mod_set_pattern_fetch_fn(uint8_t (*fetch_fn)(uint8_t pattern_num, uint8_t *dst))
{
    s_fetch_pattern = fetch_fn ? fetch_fn : disk_fetch_pattern;
    prefetch_seek_done = 0;
    prefetch_bytes_done = 0;
}

void mod_set_load_progress_plugin(const ModLoadProgressPlugin *plugin)
{
    s_progress_plugin = plugin;
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

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr)
{
    uint16_t offset = (uint16_t)row * (MOD_CHANNELS * 4u);

    if (pattern == pat_current_num) {
        *row_ptr = pat_current + offset;
        return 0;
    }
    if (pattern == pat_next_num) {
        *row_ptr = pat_next + offset;
        return 0;
    }

    *row_ptr = 0;
    return 1;
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

static uint8_t disk_fetch_pattern(uint8_t pattern_num, uint8_t *dst)
{
    uint32_t offset;
    uint16_t chunk;

    if (!mod_file) return 1;

    if (!prefetch_seek_done) {
        offset = pattern_data_offset + (uint32_t)pattern_num * (uint32_t)PAT_BYTES;
        if (fseek(mod_file, (long)offset, SEEK_SET) != 0) return 1;
        prefetch_seek_done = 1;
        prefetch_bytes_done = 0;
    }

    if (prefetch_bytes_done >= PAT_BYTES) return 0;

    chunk = (uint16_t)(PAT_BYTES - prefetch_bytes_done);
    if (chunk > 256u) chunk = 256u;

    if (fread(dst + prefetch_bytes_done, 1, chunk, mod_file) != chunk) return 1;
    prefetch_bytes_done = (uint16_t)(prefetch_bytes_done + chunk);

    return (prefetch_bytes_done >= PAT_BYTES) ? 0 : 2;
}

static uint8_t ram_fetch_pattern(uint8_t pattern_num, uint8_t *dst)
{
    uint32_t offset;

    if (!pattern_ram_cache) return 1;

    offset = (uint32_t)pattern_num * (uint32_t)PAT_BYTES;
    if (offset + (uint32_t)PAT_BYTES > (uint32_t)pattern_ram_cache_size) return 1;

    memcpy(dst, pattern_ram_cache + offset, PAT_BYTES);
    return 0;
}

/* -------------------------------------------------------
 * mod_prefetch_next_pattern() - called from MAIN LOOP only
 * Reads pat_next_num from disk into pat_next buffer.
 * ------------------------------------------------------- */
uint8_t mod_prefetch_next_pattern(void)
{
    uint8_t rc;

    if (!mod_need_prefetch)   return 0;
    if (pat_next_num == 0xFF) {
        mod_need_prefetch = 0;
        prefetch_seek_done = 0;
        prefetch_bytes_done = 0;
        return 0;
    }

    rc = s_fetch_pattern(pat_next_num, pat_next);
    if (rc == 0u) {
        mod_need_prefetch = 0;
        prefetch_seek_done = 0;
        prefetch_bytes_done = 0;
        return 0;
    }

    if (rc == 2u) {
        /* partial progress; keep mod_need_prefetch set */
        return 0;
    }

    /* error: reset partial state so a later retry restarts cleanly */
    prefetch_seek_done = 0;
    prefetch_bytes_done = 0;
    return 1;
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
    uint8_t  progress_started;
    uint8_t  had_status_output;
    uint8_t  use_adpcm_global;
    uint32_t sample_data_offset;
    uint32_t pattern_data_size;

    memset(&mod, 0, sizeof(mod));
    pat_current_num   = 0xFF;
    pat_next_num      = 0xFF;
    mod_need_prefetch = 0;

    if (pattern_ram_cache) {
        free(pattern_ram_cache);
        pattern_ram_cache = NULL;
        pattern_ram_cache_size = 0;
    }
    s_fetch_pattern = disk_fetch_pattern;

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
    pattern_data_size = (uint32_t)mod.num_patterns * (uint32_t)PAT_BYTES;

    if (pattern_data_size > 0u &&
        pattern_data_size <= (uint32_t)PATTERN_RAM_CACHE_THRESHOLD_BYTES) {
        pattern_ram_cache = (uint8_t*)malloc((size_t)pattern_data_size);
        if (pattern_ram_cache) {
            pattern_ram_cache_size = (uint16_t)pattern_data_size;
            if (fread(pattern_ram_cache, 1u, (size_t)pattern_data_size, mod_file)
                == (size_t)pattern_data_size) {
                s_fetch_pattern = ram_fetch_pattern;
            } else {
                free(pattern_ram_cache);
                pattern_ram_cache = NULL;
                pattern_ram_cache_size = 0;
                if (fseek(mod_file, (long)pattern_data_offset, SEEK_SET) != 0) return 1;
            }
        }
    }
    if (s_fetch_pattern==ram_fetch_pattern)
        printf("Using RAM for patterns\n");
    else
        printf("Streaming patterns of size %lx\n",pattern_data_size);

    use_adpcm_global = (total_sample_bytes > (uint32_t)POKEYMAX_RAM_SIZE) ? 1u : 0u;
    if (total_sample_bytes > (2*(uint32_t)POKEYMAX_RAM_SIZE))
    {
        printf("Not enough PokeyMAX ram to load samples\n");
        printf("Needed   :%6lx\nAvailable:%6lx\n",use_adpcm_global ? total_sample_bytes/2: total_sample_bytes , (int32_t)POKEYMAX_RAM_SIZE);
	exit(0);
    }

    sample_data_offset = pattern_data_offset
                       + pattern_data_size;
    if (fseek(mod_file, (long)sample_data_offset, SEEK_SET) != 0) return 1;

    pokeymax_init();
    loaded_samples      = 0;
    loaded_source_bytes = 0;
    loaded_stored_bytes = 0;
    progress_started    = 0;
    had_status_output   = 0;

    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &mod.samples[i];
        uint16_t    ram_addr, ram_needed;
        uint16_t    remaining, chunk, written, out_len;
        ADPCMState  adpcm_state;

        if (si->length == 0u) continue;

        if (!progress_started) {
            if (s_progress_plugin && s_progress_plugin->begin) {
                s_progress_plugin->begin(s_progress_plugin->ctx,
                                         total_sample_bytes);
            }
            progress_started = 1;
        }

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
            if (s_progress_plugin && s_progress_plugin->update) {
                s_progress_plugin->update(s_progress_plugin->ctx,
                                          si,
                                          (uint16_t)(loaded_samples + 1u),
                                          total_samples_to_load,
                                          loaded_source_bytes,
                                          total_sample_bytes,
                                          loaded_stored_bytes,
                                          1u);
            }
            had_status_output = 1;
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

            if (s_progress_plugin && s_progress_plugin->update) {
                s_progress_plugin->update(s_progress_plugin->ctx,
                                          si,
                                          (uint16_t)(loaded_samples + 1u),
                                          total_samples_to_load,
                                          (uint32_t)(loaded_source_bytes + (si->length - remaining)),
                                          total_sample_bytes,
                                          (uint32_t)(loaded_stored_bytes + written),
                                          0u);
            }
            had_status_output = 1;
        }

        loaded_samples++;
        loaded_source_bytes += si->length;
        loaded_stored_bytes += si->pokeymax_len;
    }

    if (had_status_output) {
        if (s_progress_plugin && s_progress_plugin->end) {
            s_progress_plugin->end(s_progress_plugin->ctx);
        }
    }

    /* Pre-load pattern for order 0 into current buffer (blocking via fetch backend) */
    {
        uint8_t rc;
        do {
            rc = s_fetch_pattern(mod.order_table[0], pat_current);
        } while (rc == 2u);
        if (rc != 0u) return 1;
        prefetch_seek_done = 0;
        prefetch_bytes_done = 0;
        pat_current_num = mod.order_table[0];
    }

    /* Pre-load pattern for order 1 into next buffer (blocking via fetch backend) */
    if (mod.song_length > 1u) {
        uint8_t rc;
        do {
            rc = s_fetch_pattern(mod.order_table[1], pat_next);
        } while (rc == 2u);
        if (rc != 0u) return 1;
        prefetch_seek_done = 0;
        prefetch_bytes_done = 0;
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
