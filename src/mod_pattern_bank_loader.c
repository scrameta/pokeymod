#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef __CC65__
#include <conio.h>
#else
typedef struct 
{
	uint8_t portb;
} PIADef;
static PIADef PIA;
#endif

#include "mod_app.h"
#include "mod_struct.h"
#include "memcpy_banked.h"

#pragma bss-name(push, "LOWBSS")
static char buffer[1024];
#pragma bss-name(pop)

void load_patterns_to_banks()
{
    // We need to tell the player where the patterns are and load them maybe
    /* Where we store the data in ram in 4k chunks: XL banks, under OS or main ram */
    /*
    uint8_t  banks;
    uint8_t  pattern_portb[8];
    uint16_t pattern_bank_addr[8];
    uint8_t pattern_row_buf[MOD_CHANNELS*4];*/
    uint8_t i;
    uint8_t f_ok;
    uint32_t pattern_data_pos;

    // Something stupid to start with
    mod.banks = mod.pattern_data_size>>12;
    if (mod.pattern_data_size&0x0fff) mod.banks+=1;   

    for (i=0;i!=64;++i)
    {
        mod.pattern_portb[i] = 0x83 | (i&0xc) |((i&0x30)<<1);
        mod.pattern_bank_addr[i] = (uint8_t *)(0x4000 + ((i&3)<<12));
    }

    // Load the pattern data
    f_ok = cio_open(mod.filename)==0;
    pattern_data_pos = 0;
    printf("Loading patterns\nsize %ld into %d banks\n",mod.pattern_data_size,mod.banks);
    if (f_ok)
    {
        cio_point(&mod.pattern_bookmark);  // instant jump via NOTE/POINT

        while (pattern_data_pos<mod.pattern_data_size)
        {
            printf("%8ld/%8ld",pattern_data_pos,mod.pattern_data_size);
#ifdef __CC65__
            gotox(0);
#else
	    PIA.portb = 0xff;
#endif	  
            i = pattern_data_pos>>12;
            if (i>=mod.banks)
            {
                printf("No enough ram for patterns");
                break;
            }
            if ((mod.pattern_portb[i]&0x11) == 0x11) // Main mem
            {
                cio_read((char *)(mod.pattern_bank_addr[i]+(pattern_data_pos&0x0fff)),1024);
            }
            else
            {
                cio_read(&buffer[0],1024);
                memcpy_banked((uint8_t *)(mod.pattern_bank_addr[i] + (uint16_t)(pattern_data_pos&0x0fff)), (const uint8_t *)&buffer[0], 1024, mod.pattern_portb[i],PIA.portb,0x40);
            }
	    pattern_data_pos+=1024;
        }
        printf("%8ld/%8ld",pattern_data_pos,mod.pattern_data_size);

        cio_close();
    }
}

