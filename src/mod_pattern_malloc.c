#include <stdlib.h>
#include <stdio.h>

#include "mod_pattern.h"
#include "mod_struct.h"

static char * pattern_buf = 0;

//#define PATTERN_DEBUG

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr)
{

#ifdef PATTERN_DEBUG
	printf("Get %d from %d of %s - pattern:%d.%d\n",mod.pattern_data_size,mod.pattern_data_offset,mod.filename,pattern,row);
#endif
	
#ifdef PATTERN_DEBUG
	printf("Pattern %d.%d\n",pattern,row);
#endif

	*row_ptr = (uint8_t const *) pattern_buf + pattern*PAT_BYTES + MOD_CHANNELS*4*row;

	return 0;
}

void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next)
{
#ifdef PATTERN_DEBUG
	printf("Advance %d->%d\n",new_current, prefetch_next);
#endif
	mod_need_prefetch = 0;
}

void mod_pattern_init(uint8_t new_current, uint8_t prefetch_next)
{
	if (!pattern_buf)
	{
#ifdef PATTERN_DEBUG
		printf("Reading pattern data from disk. %s:%d@%d\n",mod.filename,mod.pattern_data_size,mod.pattern_data_offset);
#endif
		pattern_buf = (char*)malloc(mod.pattern_data_size);
		FILE * f = fopen(mod.filename,"rb");
		fseek(f, mod.pattern_data_offset, SEEK_SET);
		fread(pattern_buf,mod.pattern_data_size,1,f);
		fclose(f);
	}
	mod_need_prefetch = 0;
}


