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

//#pragma bss-name(push, "PATTERNBSS")
//static char pattern_buffer1[4096];
//static char pattern_buffer2[4096];
//static char pattern_buffer3[4096];
//#pragma bss-name(pop)

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
    uint8_t pattern_base=0;
    uint8_t * internal_ram_base=0;
    uint8_t no_os_portb=0x72;
    uint8_t val=0;

    // Something stupid to start with
    mod.banks = mod.pattern_data_size>>12;
    if (mod.pattern_data_size&0x0fff) mod.banks+=1;   

#ifdef __CC65__
    /* Bank 0 uses reserved high RAM at $7C00-$8BFF.
       Then behind the OS
       Remaining banks use the normal 4x4KiB $4000-$7FFF bank window. */
    internal_ram_base= 1+ *(uint16_t *)(0xe); //reserved 12k after app

    // 12KB RAM in 48KB systems
    mod.pattern_portb[pattern_base] = 0xFF;
    mod.pattern_bank_addr[pattern_base] = internal_ram_base;
    mod.pattern_portb[pattern_base+1] = 0xFF;
    mod.pattern_bank_addr[pattern_base+1] = internal_ram_base+4096;
    mod.pattern_portb[pattern_base+2] = 0xFF;
    mod.pattern_bank_addr[pattern_base+2] = internal_ram_base+8192;
    pattern_base += 3;

    // OS = 0xc000 to 0xffff 
    // Hardware = 0xd000 to 0xd7ff
    // So we can use (4k chunks): 0xc000 to 0xcfff, 0xd800 to 0xe7ff, 0xe800 to 0xf7ff
    // 0x2f4 is chbase, normally E0 -> this will blip out from time to time
    // Copy chset fom 0xE0 to 0xf8 
    // memcpy_banked(0xf800, 0xe000, 1024, no_os_portb, PIA.portb, 0x40);
    memcpy_banked(0xf800, 0xe000, 10, no_os_portb, PIA.portb, 0x40);
    memcpy_banked(&val, 0xf809, 1, no_os_portb, PIA.portb, 0x40);
    if (val==0x18)
    {
	printf("Detected 64KB\n");
        mod.pattern_portb[pattern_base] = no_os_portb;
        mod.pattern_bank_addr[pattern_base] = 0xc000;
        mod.pattern_portb[pattern_base+1] = no_os_portb;
        mod.pattern_bank_addr[pattern_base+1] = 0xd800;
        mod.pattern_portb[pattern_base+2] = no_os_portb;
        mod.pattern_bank_addr[pattern_base+2] = 0xe800;
        pattern_base += 3;
    }
#endif

    // Then bank switching
    // TODO detect banks -- I need to have a val to compare with? So I need to be able to write to 0x4000 to 0x7fff somewhere
    for (i=pattern_base;i<(64+pattern_base);++i)
    {
        uint8_t j = (uint8_t)(i - pattern_base);
        mod.pattern_portb[i] = 0x83 | (j&0x0c) |((j&0x30)<<1);
        mod.pattern_bank_addr[i] = (uint8_t *)(0x4000 + ((j&0x03)<<12));
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

