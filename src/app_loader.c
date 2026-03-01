#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"
#include "mod_app.h"

static uint8_t pokeymax_detect(void)
{
    uint8_t cap;

    if (PEEK(REG_CFGID) != 1u) return 0;

    POKE(REG_CFGUNLOCK, 0x3F);
    cap = PEEK(REG_CFG_CAP);
    POKE(REG_CFGUNLOCK, 0x00);

    if (!(cap & CAP_SAMPLE)) return 1;
    return 2;
}

static void display_load_summary(void)
{
    uint8_t i;
    uint8_t loaded = 0, adpcm = 0, looped = 0;
    uint32_t raw_bytes = 0, hw_bytes = 0;

    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        const SampleInfo *si = &mod.samples[i];
        if (si->length == 0u) continue;
        loaded++;
        if (si->is_adpcm) adpcm++;
        if (si->has_loop) looped++;
        raw_bytes += si->length;
        hw_bytes  += si->pokeymax_len;
    }

    printf("Samples:  %u loaded (%u ADPCM, %u looped)\n",
           (unsigned)loaded, (unsigned)adpcm, (unsigned)looped);
    printf("Audio:    %lu bytes raw -> %lu bytes\n",
           (unsigned long)raw_bytes, (unsigned long)hw_bytes);
}

uint8_t app_loader_run(const char *filename, uint8_t show_progress_ui)
{
    uint8_t det;

    (*(volatile unsigned char*)0x41) = 0; /* disable beeps */

    clrscr();
    printf("PokeyMAX MOD Player\n");
    printf("-------------------\n");

    det = pokeymax_detect();
    if (det == 0u) { printf("Requires PokeyMAX.\n"); return 1; }
    if (det == 1u) { printf("PokeyMax has no sample player.\n"); return 1; }

    if (show_progress_ui) {
        mod_set_load_progress_plugin(&mod_default_load_progress_plugin);
    } else {
        mod_set_load_progress_plugin(0);
    }

    printf("Loading: %s\n", filename);
    if (mod_load(filename) != 0u) { printf("Load failed.\n"); return 1; }

    printf("Orders:   %d\n", (int)mod.song_length);
    printf("Patterns: %d\n", (int)mod.num_patterns);
    printf("RAM used: %u / %u\n",
           (unsigned)pokeymax_ram_ptr,
           (unsigned)POKEYMAX_RAM_SIZE);
    display_load_summary();
    printf("\nSPACE=pause  any key=stop\n\n");

    return 0;
}
