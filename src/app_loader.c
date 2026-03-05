#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "mod_app.h"
#include "mod_struct.h"
#include "mod_loader.h"
#include "mod_default_progress_plugin.h"

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

    return 0;
}
