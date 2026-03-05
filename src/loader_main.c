#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>

#include "mod_app.h"
#include "mod_struct.h"
#include "memcpy_banked.h"

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

#pragma bss-name(push, "LOWBSS")
static char buffer[1024];
#pragma bss-name(pop)

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t bank_first = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_FIRST;
    uint8_t bank_count = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_COUNT;
    int i;
    FILE * f;
    uint32_t pattern_data_pos;

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

    if (app_loader_run(filename, 1u) != 0u) {
        return 1;
    }

    // We need to tell the player where the patterns are and load them maybe
    /* Where we store the data in ram in 4k chunks: XL banks, under OS or main ram */
    /*
    uint8_t  banks;
    uint8_t  pattern_portb[8];
    uint16_t pattern_bank_addr[8];
    uint8_t pattern_row_buf[MOD_CHANNELS*4];*/
    // Something stupid to start with
    mod.banks = mod.pattern_data_size>>12;
    if (mod.pattern_data_size&0x0fff) mod.banks+=1;   

    for (i=0;i!=64;++i)
    {
        mod.pattern_portb[i] = 0xc3 | (i&0x3c); 
        mod.pattern_bank_addr[i] = 0x4000 + ((i&3)<<12);
    }

    // Load the pattern data
    f = fopen(mod.filename,"rb");
    pattern_data_pos = 0;
    printf("Loading patterns\nsize %ld into %d banks\n",mod.pattern_data_size,mod.banks);
    if (f)
    {
        fseek(f, mod.pattern_data_offset, SEEK_SET);

        while (pattern_data_pos<mod.pattern_data_size)
        {
            printf("%8ld/%8ld",pattern_data_pos,mod.pattern_data_size);
            gotox(0);
            i = pattern_data_pos>>12;
            if (i>=mod.banks)
            {
                printf("No enough ram for patterns");
                break;
            }
            if ((mod.pattern_portb[i]&0x11) == 0x11) // Main mem
            {
                fread((char *)(mod.pattern_bank_addr[i]+pattern_data_pos&0x0fff),1024,1,f);
            }
            else
            {
		uint16_t pattern_data_pos_short = pattern_data_pos;
                fread(&buffer[0],1024,1,f);
                memcpy_banked((uint8_t *)(mod.pattern_bank_addr[i] + (pattern_data_pos_short&0x0fff)), (const uint8_t *)&buffer[0], 1024, mod.pattern_portb[i],0xff,0x40);
            }
	    pattern_data_pos+=1024;
        }
        printf("%8ld/%8ld",pattern_data_pos,mod.pattern_data_size);

        fclose(f);
    }
    printf("Loading player\n");

    return 0;
}
