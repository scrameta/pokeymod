#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "mod_app.h"
#include "mod_loader.h"
#include "mod_struct.h"
#include "mod_pattern_bank_loader.h"
#include "sdx_path.h"

static void usage(void)
{
    printf("Usage: modplay.xex [/N] [/T] [modfile]\n");
    printf("  /N  legacy NTSC speed-only tempo\n");
    printf("  /T  POKEY timer IRQ tick timing\n");
    printf("  /?  show this help\n");
}

static uint8_t is_option(const char *arg, char opt)
{
    return arg[0] == '/' &&
           (arg[1] == opt || arg[1] == (char)(opt + ('a' - 'A'))) &&
           arg[2] == '\0';
}

static void quit_to_dos(void)
{
    /* In concatenated-XEX builds, returning from the loader lets the next
     * RUN/INIT segment continue.  Jump DOSVEC instead so help/load errors
     * leave the XEX stream completely. */
    __asm__("jmp ($000A)");
    for (;;) { }
}

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    char resolved[64];
    int i;

    for (i = 1; i < argc; i++) {
        if (is_option(argv[i], 'N')) {
            mod_set_legacy_tempo_pal(0u);
        } else if (is_option(argv[i], 'T')) {
            mod_set_timer_timing(1u);
        } else if (argv[i][0] == '/' && argv[i][1] == '?' && argv[i][2] == '\0') {
            usage();
            quit_to_dos();
        } else if (argv[i][0] == '/' || argv[i][0] == '-') {
            printf("Bad option: %s\n", argv[i]);
            usage();
            quit_to_dos();
        } else {
            filename = argv[i];
        }
    }

    sdx_resolve_path(filename, resolved, 64);
    if (app_loader_run(resolved, 1u) != 0u) {
        quit_to_dos();
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
