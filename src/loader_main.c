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


int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    char resolved[64];
    int i;

    for (i = 1; i < argc; i++) {

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
