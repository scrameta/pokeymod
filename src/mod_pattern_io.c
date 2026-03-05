#include <stdio.h>
#include <stdlib.h>

#include "mod_pattern_io.h"
#include "mod_struct.h"

static FILE * file = 0;
static char * pat_buf1 = 0;
static char * pat_buf2 = 0;
static uint8_t pat1 = 255;
static uint8_t pat2 = 255;
static uint8_t valid = 0;
static uint8_t needed_first = 255;
static uint8_t needed_second = 255;

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr)
{
	uint16_t offset = ((uint16_t)row*MOD_CHANNELS*4);
	uint8_t section = row>>4;
	uint8_t req = 1<<section;
	char const * src = 0;

	if (pattern == pat1)
	{
		src = pat_buf1+offset;
	}
	else if (pattern == pat2)
	{
		req = req<<4;
		src = pat_buf2+offset;
	}
	else
	{
		return 1;
	}
	if (!(valid&req))
		return 2;

	uint8_t len = MOD_CHANNELS*4;
	char * dst = &mod.pattern_row_buf[0];

        while (len > 0u) {
        	*dst++ = *src++;
        	--len;
        }

	*row_ptr = &mod.pattern_row_buf[0];

	return 0;
}

void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next)
{
	uint8_t have_first = 0;
	uint8_t have_second = 0;
	uint8_t need_buf1 = 0;
	uint8_t need_buf2 = 0;
	needed_first = new_current;
	needed_second = prefetch_next;

	// Do we have first already?
	if (new_current == pat1)
	{
		have_first = 1;
		need_buf1 = 1;
	}
	else if (new_current == pat2)
	{
		have_first = 1;
		need_buf2 = 1;
	}

	// Do we have second already?
	if (prefetch_next == pat1)
	{
		have_second = 1;
		need_buf1 = 1;
	}
	else if (prefetch_next == pat2)
	{
		have_second = 1;
		need_buf2 = 1;
	}

	if (!need_buf1)
	{
		pat1 = 0xff;
		valid = valid & 0xf0;
		if (!have_first)
		{
			pat1 = needed_first;
			have_first = 1;
		}
		else if (!have_second)
		{
			pat1 = needed_second;
			have_second = 1;
		}
	}
	if (!need_buf2)
	{
		pat2 = 0xff;
		valid = valid & 0x0f;
		if (!have_first)
		{
			pat2 = needed_first;
			have_first = 1;
		}
		else if (!have_second)
		{
			pat2 = needed_second;
			have_second = 1;
		}
	}

	mod_need_prefetch = ((pat1!=0xff) && ((valid&0xf) != 0xf)) || ((pat2!=0xff) && ((valid&0xf0) != 0xf0));
}

//  TODO:Not threaded but I can get asked for something else in the middle of this in VBI
void load_pattern()
{
	char * buf = 0;
	uint8_t pattern = 255;
	uint8_t bit = 0;
	uint16_t offset = 0;
	uint8_t valid_shift = 0;
	uint8_t valid_adj = 0;
	if (!file)
	{
		file = fopen(mod.filename,"rb");
		pat_buf1 = (char *)malloc(2*PAT_BYTES);
		pat_buf2 = pat_buf1+PAT_BYTES;
	}

	if (needed_first == pat1 && ((valid&0x0f) != 0x0f))
	{
		buf = pat_buf1;
		pattern = pat1;
		valid_shift = 0;
	}
	else if (needed_first == pat2 && ((valid&0xf0) != 0xf0))
	{
		buf = pat_buf2;
		pattern = pat2;
		valid_shift = 4;
	}
	else if (needed_second == pat1 && ((valid&0x0f) != 0x0f))
	{
		buf = pat_buf1;
		pattern = pat1;
		valid_shift = 0;
	}
	else if (needed_second == pat2 && ((valid&0xf0) != 0xf0))
	{
		buf = pat_buf2;
		pattern = pat2;
		valid_shift = 4;
	}

	if (pattern == 255)
	{
		mod_need_prefetch = 0;
		return;
	}

	valid_adj = (valid>>valid_shift)&0xf;
	bit = ((valid_adj + 1) >> 1) - (valid_adj >> 2);

	offset = ((uint16_t)bit) << 8;

	fseek(file, mod.pattern_data_offset+((uint16_t)pattern)*PAT_BYTES+offset, SEEK_SET);
	fread(buf+offset,256,1,file);
	valid |= 1<<(bit+valid_shift);
}

void mod_pattern_init(uint8_t new_current, uint8_t prefetch_next)
{
	mod_pattern_advance(new_current,prefetch_next);
	load_pattern();
}
