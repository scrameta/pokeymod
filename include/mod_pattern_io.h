#ifndef MODPATTERN_IO_H
#define MODPATTERN_IO_H
#include "mod_pattern.h"


/*
 * Pattern RAM strategy (no VBI disk I/O):
 *
 *   Two 1KB pattern buffers. 'current' is what the VBI reads (memcpy only,
 *   no disk). 'next' is filled by the main loop between VBIs.
 *
 *   When the player advances to a new pattern, mod_pattern_advance() swaps
 *   the pointers and sets mod_need_prefetch=1. The main loop then calls
 *   load_pattern() which does the actual fseek+fread.
 *
 *   The 1050 disk drive takes ~100ms to seek + read 1KB. At 125BPM/speed6
 *   a pattern lasts ~7.5 seconds, so there is ample time.
 *
 *   Total pattern RAM: 2 x 1024 = 2KB. Works on stock 64KB 800XL.
 */

void load_pattern(); // Call in the main loop, might load a sector from 1050...


#endif //MODPATTERN_IO_H
