#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "mod_app.h"
#include "mod_struct.h"
#include "mod_pattern_bank_loader.h"
#include "sdx_path.h"

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_FIRST
#define MODPLAY_DEFAULT_PATTERN_BANK_FIRST 0u
#endif

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_COUNT
#define MODPLAY_DEFAULT_PATTERN_BANK_COUNT 0u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_FIRST
#define MODPLAY_AUTO_PATTERN_BANK_FIRST 0u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_COUNT
#define MODPLAY_AUTO_PATTERN_BANK_COUNT 4u
#endif

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t bank_first = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_FIRST;
    uint8_t bank_count = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_COUNT;
    char resolved[64];
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-B=", 3) == 0) {
            unsigned long n = strtoul(argv[i] + 3, 0, 10);
            bank_first = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_FIRST;
            bank_count = (uint8_t)n;
            continue;
        }

        if (strcmp(argv[i], "-B") == 0) {
            bank_first = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_FIRST;
            bank_count = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_COUNT;

            continue;
        }

        filename = argv[i];
    }

    sdx_resolve_path(filename, resolved, 64);
    if (app_loader_run(resolved, 1u) != 0u) {
        return 1;
    }

    load_patterns_to_banks();

    clrscr();	    
    printf("Loading player...\n");

    // Fix the sector buffer, so we can chain load!
    {
        uint8_t *iocb = (uint8_t *)0x0350;
        uint8_t dummy[256];
        /* NOTE current position */
        iocb[2] = 0x26;
        __asm__("ldx #$10");
        __asm__("jsr $E456");
        /* Read a full sector to force buffer reload */
        iocb[2] = 0x07;
        iocb[4] = (uint8_t)((uint16_t)dummy & 0xFF);
        iocb[5] = (uint8_t)((uint16_t)dummy >> 8);
        iocb[8] = 0;    /* 256 bytes (low=0, high=1) */
        iocb[9] = 1;
        __asm__("ldx #$10");
        __asm__("jsr $E456");
        /* POINT back */
        iocb[2] = 0x25;
        __asm__("ldx #$10");
        __asm__("jsr $E456");
    }

    return 0;
}
