#include "memcpy_banked.h"
#include <string.h>
#include <stdlib.h>

uint8_t * buffer = 0;

void memcpy_banked(uint8_t *dst, const uint8_t *src,
                               uint16_t len,
                               uint8_t new_portb,
                               uint8_t restore_portb,
                               uint8_t restore_nmien)
{
    if (buffer==0) buffer = (uint8_t *)malloc(262144);
    (void)restore_portb;
    (void)restore_nmien;
    uint8_t const * src2 = src;
    if (src < (uint8_t *) 65536)
        src2 += (int64_t)buffer + 65536 + 16384*(((new_portb&0xc) >> 2)  + ((new_portb&0x60)>>5));
    uint8_t * dst2 = dst;
    if (dst < (uint8_t *) 65536 )
        dst2 += (int64_t)buffer + 65536 + 16384*(((new_portb&0xc) >> 2)  + ((new_portb&0x60)>>5));
    memcpy(dst2, src2, len);
}
