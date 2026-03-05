#include <stdio.h>
#include <stdlib.h>

#include "mod_pattern.h"
#include "mod_struct.h"
#include "memcpy_banked.h"

//Where we store the data in ram in 4k chunks: XL banks, under OS or main ram 
//uint8_t  pattern_portb[8];
//uint16_t pattern_bank_addr[8];

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr)
{
	uint16_t offset = ((uint16_t)pattern)*PAT_BYTES + ((uint16_t)row*MOD_CHANNELS*4);
	uint8_t bank = offset>>12;
	uint8_t portb_to_use;
	uint16_t addr_to_use;
	uint16_t bank_offset = (offset&0x0fff);
	if (bank>mod.banks) return 1;
	if (offset>mod.pattern_data_size) return 2;

	portb_to_use = mod.pattern_portb[bank];
	addr_to_use = mod.pattern_bank_addr[bank];
	if ((portb_to_use&0x11) == 0x11) // Main mem
		*row_ptr = (uint8_t const *) (addr_to_use + bank_offset);
	else
	{
		uint8_t const*src = (uint8_t const*)(addr_to_use + bank_offset);
		uint8_t *dst = &mod.pattern_row_buf[0];
		uint8_t len = MOD_CHANNELS*4;

		memcpy_banked(dst,src,len,portb_to_use,0xff,0x40);
		
		*row_ptr = &mod.pattern_row_buf[0];
	}

	return 0;
}

void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next)
{
}

void mod_pattern_init(uint8_t new_current, uint8_t prefetch_next)
{
}

