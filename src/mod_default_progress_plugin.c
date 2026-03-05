
#include <stdint.h>
#include <stdio.h>
#if defined(__CC65__)
#include <conio.h>
#endif

#include "mod_default_progress_plugin.h"
#include "mod_format.h"
#include "pokeymax_hw.h"

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
    uint8_t  downsample_factor;
    uint8_t  skipped;
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
    ui->downsample_factor = 0xFFu;
    ui->skipped       = 0xFFu;

    printf("Load 00/00   0%%\n");
    printf("Fmt:-----/- Len:00000 St:-------\n");
    printf("Src:%8lu/%8lu\n", 0UL, (unsigned long)source_total);
    printf("Dst:00000000/%8lu\n",(unsigned long)POKEYMAX_RAM_SIZE);
    ui->source_total = source_total;
    fflush(stdout);
#else
    (void)ui;
    printf("\r[ 0/ 0] %-5s  len:%5u  src:%9lu/%9lu  dst:%9lu  %-7s   ",
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
    const char *fmt  = SI_IS_ADPCM(si) ? "ADPCM" : "PCM";
    const char *st   = skipped ? "SKIPPED" : "OK";
    uint8_t pct      = pct_u8(source_loaded, source_total);
    uint8_t ds_shift = SI_DS_SHIFT(si);
    uint8_t ds_factor = (uint8_t)(1u << ds_shift);  /* 1, 2, 4, or 8 */

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

    if (SI_IS_ADPCM(si) != ui->is_adpcm) {
        gotoxy(4, ui->base_row + 1u);
        printf("%-5s", fmt);
        ui->is_adpcm = SI_IS_ADPCM(si);
    }

    if (ds_factor != ui->downsample_factor) {
        gotoxy(10, ui->base_row + 1u);
        printf("%1d", ds_factor);
        ui->downsample_factor = ds_factor;
    }

    if (si->length != ui->sample_len) {
        gotoxy(16, ui->base_row + 1u);
        printf("%5u", (unsigned)si->length);
        ui->sample_len = si->length;
    }

    if (skipped != ui->skipped) {
        gotoxy(25, ui->base_row + 1u);
        printf("%-7s", st);
        ui->skipped = skipped;
    }

    if (source_loaded != ui->source_loaded) {
        gotoxy(4, ui->base_row + 2u);
        printf("%8lu", (unsigned long)source_loaded);
        ui->source_loaded = source_loaded;
    }

    if (source_total != ui->source_total) {
        gotoxy(13, ui->base_row + 2u);
        printf("%8lu", (unsigned long)source_total);
        ui->source_total = source_total;
    }

    if (stored_loaded != ui->stored_loaded) {
        gotoxy(4, ui->base_row + 3u);
        printf("%8lu", (unsigned long)stored_loaded);
        ui->stored_loaded = stored_loaded;
    }

    fflush(stdout);
#else
    (void)ui;
    printf("\r[%2u/%2u] %-5s  len:%5u  src:%9lu/%9lu  dst:%9lu  %-7s   ",
           (unsigned)sample_index,
           (unsigned)total_samples,
           SI_IS_ADPCM(si) ? "ADPCM" : "PCM",
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

