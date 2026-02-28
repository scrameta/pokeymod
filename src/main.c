/*
 * main.c - PokeyMAX MOD Player
 *
 * Keyboard: reads OS variable CH ($02FC) directly.
 * CH holds the last key pressed (ATASCII value); 255 = no key.
 * This avoids CIO/kbhit() conflicts with our IRQ hook.
 *
 * Build:
 *   cl65 -t atari -O -o modplay.xex \
 *        src/main.c src/modplayer.c src/mod_loader.c \
 *        src/pokeymax_hw.c src/loop_handler.c \
 *        src/adpcm.c src/tables.c src/vbi_handler.s \
 *        -I include
 */

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <atari.h>
#include <stdint.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

extern void vbi_install(void);
extern void vbi_remove(void);

/* -------------------------------------------------------
 * Keyboard via OS CH variable - no CIO, no conflict
 * CH = $02FC, holds ATASCII of last key, 255 = none
 * ------------------------------------------------------- */
#define MOD_KEY_NONE  255
#define MOD_KEY_SPACE  32

static uint8_t key_read_clear(void)
{
    uint8_t k;
    k = PEEK(0x02FC);
    POKE(0x02FC, MOD_KEY_NONE);  /* clear after reading */
    return k;
}

/* -------------------------------------------------------
 * Wait exactly one VBI frame using RTCLOK at $14
 * (the least-significant byte of the 3-byte clock at $12)
 * ------------------------------------------------------- */
static void wait_vbi(void)
{
    uint8_t t;
    t = PEEK(RTCLOK);
    while (PEEK(RTCLOK) == t) ;
}

/* -------------------------------------------------------
 * PokeyMAX detect + capability check
 * Returns 0=not found, 1=found/no sample, 2=found/sample OK
 * ------------------------------------------------------- */
static uint8_t pokeymax_detect(void)
{
    uint8_t cap;
    uint8_t pokeys;

    /* Step 1: read ID register (no unlock needed, always returns 1 if present) */
    if (PEEK(REG_CFGID) != 1u) return 0;

    /* Step 2: unlock config */
    POKE(REG_CFGUNLOCK, 0x3F);

    /* Step 3: read capability */
    cap = PEEK(REG_CFG_CAP);

    /* Step 4: re-lock */
    POKE(REG_CFGUNLOCK, 0x00);

    pokeys = cap & CAP_POKEY_MASK;
    if      (pokeys == 0u) printf("  Mono POKEY\n");
    else if (pokeys == 1u) printf("  Stereo POKEY\n");
    else                   printf("  Quad POKEY\n");

    if (cap & CAP_SID)   printf("  SID\n");
    if (cap & CAP_PSG)   printf("  PSG\n");
    if (cap & CAP_COVOX) printf("  COVOX\n");
    if (cap & CAP_FLASH) printf("  Flash\n");

    if (!(cap & CAP_SAMPLE)) {
        printf("  No sample player\n");
        return 1;
    }
    printf("  Sample player\n");
    return 2;
}

/* -------------------------------------------------------
 * Status line
 * ------------------------------------------------------- */

static void display_load_summary(void)
{
    uint8_t i;
    uint8_t loaded = 0, adpcm = 0, looped = 0;
    uint8_t shown = 0;
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
    printf("Audio:    %lu bytes raw -> %lu bytes PokeyMAX\n",
           (unsigned long)raw_bytes, (unsigned long)hw_bytes);

    printf("Sample list (first 8 loaded):\n");
    for (i = 1; i <= MOD_MAX_SAMPLES && shown < 8u; i++) {
        const SampleInfo *si = &mod.samples[i];
        if (si->length == 0u) continue;
        printf("  %2u %-22s len=%5u %s%s\n",
               (unsigned)i,
               si->name[0] ? si->name : "(unnamed)",
               (unsigned)si->length,
               si->is_adpcm ? "ADPCM" : "PCM",
               si->has_loop ? " loop" : "");
        shown++;
    }
    if (loaded > shown) printf("  ... (%u more)\n", (unsigned)(loaded - shown));
}

static void display_status(void)
{
    gotoxy(0,22);	
    printf("Ord:%3d/%3d Row:%2d BPM:%3d Spd:%d  \r",
           (int)(mod_get_order() + 1),
           (int)mod.song_length,
           (int)mod_get_row(),
           (int)mod.bpm,
           (int)mod.speed);
}

/* -------------------------------------------------------
 * main
 * ------------------------------------------------------- */
int main(int argc, char *argv[])
{
    const char *filename;
    uint8_t     key;
    uint8_t     paused;
    uint8_t     det;

    filename = (argc > 1) ? argv[1] : "D1:MOD.DAT";
    paused   = 0;

    clrscr();
    printf("PokeyMAX MOD Player\n");
    printf("-------------------\n");

    printf("Detecting PokeyMAX...\n");
    det = pokeymax_detect();
    if (det == 0u) { printf("Not found.\n"); return 1; }
    if (det == 1u) { printf("No sample player.\n"); return 1; }

    printf("Loading: %s\n", filename);
    if (mod_load(filename) != 0u) { printf("Load failed.\n"); return 1; }

    printf("Orders:   %d\n", (int)mod.song_length);
    printf("Patterns: %d\n", (int)mod.num_patterns);
    printf("RAM used: %u / %u\n",
           (unsigned)pokeymax_ram_ptr,
           (unsigned)POKEYMAX_RAM_SIZE);
    display_load_summary();
    printf("\nSPACE=pause  any key=stop\n\n");

    /* Clear any stale keypress */
    POKE(0x02FC, MOD_KEY_NONE);

    vbi_install();
    mod_play();

    while (mod.playing) {
        display_status();

        /* Prefetch next pattern from disk if needed (MUST be in main loop, not VBI) */
        if (mod_need_prefetch) {
            mod_prefetch_next_pattern();
        }

        key = key_read_clear();
        if (key != MOD_KEY_NONE) {
            if (key == MOD_KEY_SPACE) {
                paused = (uint8_t)!paused;
                mod_pause();
                if (paused) printf("\nPAUSED\n");
                else        printf("\n");
            } else {
                break;  /* any other key = stop */
            }
        }

        wait_vbi();
    }

    mod_stop();
    vbi_remove();
    mod_file_close();

    printf("\nStopped.\n");
    return 0;
}
