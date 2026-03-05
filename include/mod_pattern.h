#ifndef MODPATTERN_H
#define MODPATTERN_H

#include <stdint.h>

#include "mod_format.h"

/*
 * Pattern fetch strategy - called in the VBI, just return a pointer to the requested row
 */

uint8_t mod_get_row_ptr(uint8_t pattern, uint8_t row, const uint8_t **row_ptr);

/*
 * mod_pattern_advance()
 * Called from VBI when order position changes.
 * If you need time to load data then set the mod_need_prefetch and clear it when you have the data.
 */
void mod_pattern_advance(uint8_t new_current, uint8_t prefetch_next);

extern uint8_t mod_need_prefetch;

/*
 * mod_pattern_init()
 * Called from mod_play to init 
 */
void mod_pattern_init(uint8_t new_current, uint8_t prefetch_next);

#endif /* MODPATTERN_H */
