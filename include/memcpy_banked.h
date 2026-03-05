#ifndef MEMCPY_BANKED_H
#define MEMCPY_BANKED_H

#include <stdint.h>

/* Copy len bytes from src to dst under a different PORTB bank.
 *
 * new_portb:      PORTB value to switch to before copy
 * restore_portb:  PORTB shadow value to restore after copy
 * restore_nmien:  NMIEN shadow value to restore after copy
 *
 * Both PORTB ($D301) and NMIEN ($D40E) are write-only -- callers must
 * maintain their own shadows and pass them in here rather than reading
 * the hardware registers.
 *
 * This function and its data must be in LOWCODE/LOWBSS segments placed
 * below $4000 in the linker config, so the code remains mapped regardless
 * of which extended RAM bank is active.
 */
void __fastcall__ memcpy_banked(uint8_t *dst, uint8_t const *src,
                                 uint16_t len,
                                 uint8_t new_portb,
                                 uint8_t restore_portb,
                                 uint8_t restore_nmien);

#endif /* MEMCPY_BANKED_H */
