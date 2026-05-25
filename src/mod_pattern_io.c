/*
 * load_pattern() — foreground pattern prefetch.
 *
 * Each 1024-byte pattern is divided into four 256-byte sections (16 rows
 * each), tracked by the 4-bit valid bitmap.  To avoid blocking the VBI
 * with a single large CIO read, each section is further split into
 * LOAD_CHUNK sub-reads.  Each call to load_pattern() performs exactly
 * one sub-read, so the caller must invoke it repeatedly (once per
 * main-loop iteration is fine).
 *
 * The fseek is performed only on the first sub-chunk of a section;
 * subsequent sub-chunks use sequential fread.
 *
 * The VBI can call mod_pattern_advance() at any moment (NMI), so
 * shared state is snapshotted/updated with NMIEN briefly disabled.
 */

#include <stdio.h>
#include <stdlib.h>

#include "mod_pattern_io.h"
#include "mod_struct.h"

static uint8_t f_ok = 0;
static char * pat_buf1 = 0;
static char * pat_buf2 = 0;
static uint8_t pat1 = 255;
static uint8_t pat2 = 255;
static uint8_t valid = 0;
static uint8_t needed_first = 255;
static uint8_t needed_second = 255;

#define LOAD_CHUNK   128u                 /* bytes per call (one SD sector) */
#define LOAD_SUBS    (256u / LOAD_CHUNK)  /* sub-reads per section */

static uint8_t load_sub = 0;  /* 0 .. LOAD_SUBS-1: current sub-chunk */

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr)
{
	uint16_t offset = ((uint16_t)row*MOD_CHANNELS*4);
	uint8_t section = row>>4;
	uint8_t req = 1<<section;
	char const * src = 0;
	uint8_t len = MOD_CHANNELS*4;
	char * dst = &mod.pattern_row_buf[0];

	printf("Fetching pattern %d row %d\n",pattern,row);

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

	/* Any in-progress sub-chunk load is now stale; the next
	 * foreground load_pattern() call must start fresh. */
	load_sub = 0;

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

void load_pattern()
{
	char * buf = 0;
	uint8_t pattern = 255;
	uint8_t bit = 0;
	uint16_t section_offset = 0;
	uint8_t valid_shift = 0;
	uint8_t valid_adj = 0;
	 
	/* Snapshot of shared state taken under NMIEN=0 */
	uint8_t s_pat1, s_pat2, s_valid, s_nf, s_ns;

	if (!f_ok)
	{
		f_ok = cio_open(mod.filename)==0;
		pat_buf1 = (char *)malloc(2*PAT_BYTES);
		pat_buf2 = pat_buf1+PAT_BYTES;
	}

	/* --- Snapshot shared state (VBI cannot run while NMIEN=0) --- */
#ifdef __CC65__
	POKE(0xD40E, 0x00);
#endif
	s_pat1  = pat1;
	s_pat2  = pat2;
	s_valid = valid;
	s_nf    = needed_first;
	s_ns    = needed_second;
#ifdef __CC65__
	POKE(0xD40E, 0x40);
#endif

	if (s_nf == s_pat1 && ((s_valid&0x0f) != 0x0f))
	{
		buf = pat_buf1;
		pattern = s_pat1;
		valid_shift = 0;
	}
	else if (s_nf == s_pat2 && ((s_valid&0xf0) != 0xf0))
	{
		buf = pat_buf2;
		pattern = s_pat2;
		valid_shift = 4;
	}
	else if (s_ns == s_pat1 && ((s_valid&0x0f) != 0x0f))
	{
		buf = pat_buf1;
		pattern = s_pat1;
		valid_shift = 0;
	}
	else if (s_ns == s_pat2 && ((s_valid&0xf0) != 0xf0))
	{
		buf = pat_buf2;
		pattern = s_pat2;
		valid_shift = 4;
	}

	if (pattern == 255)
	{
		mod_need_prefetch = 0;
		load_sub = 0;
		return;
	}

	/* Which 256-byte section are we loading? */
	valid_adj = (s_valid>>valid_shift)&0xf;
	bit = ((valid_adj + 1) >> 1) - (valid_adj >> 2);
	section_offset = ((uint16_t)bit) << 8;

	/* First sub-chunk of a section: seek to the right file position */
	if (load_sub == 0)
	{
		cio_seek(mod.filename, mod.pattern_data_offset
		            + ((uint16_t)pattern) * PAT_BYTES
		            + section_offset,
		      SEEK_SET,&mod.pattern_bookmark
		      );
	}

	/* Read one sub-chunk (sequential after the initial seek) */
	cio_read(buf + section_offset + (uint16_t)load_sub * LOAD_CHUNK,
	      LOAD_CHUNK);

	load_sub++;

        printf("Loading\n");
	if (load_sub >= LOAD_SUBS)
	{
		/* Section complete — mark valid */
		uint8_t valid_bit = (uint8_t)(1u << (bit + valid_shift));
		load_sub = 0;

#ifdef __CC65__
		POKE(0xD40E, 0x00);
#endif
		valid |= valid_bit;
		mod_need_prefetch = ((pat1!=0xff) && ((valid&0xf) != 0xf))
		                 || ((pat2!=0xff) && ((valid&0xf0) != 0xf0));
#ifdef __CC65__
		POKE(0xD40E, 0x40);
#endif
	}
}

void mod_pattern_init(uint8_t new_current, uint8_t prefetch_next)
{
	uint8_t i;
	load_sub = 0;
	mod_pattern_advance(new_current,prefetch_next);
	/* 2 buffers * 4 sections * LOAD_SUBS sub-chunks each */
	for (i = 0; i != 8u * LOAD_SUBS; ++i)
		load_pattern();
}
