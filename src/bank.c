#include <atari.h>
#include <stdint.h>

#pragma code-name(push, "LOWCODE")
void bank_copy_from_window(uint8_t *dst, uint16_t bank_offset, uint16_t len, uint8_t bank_portb)
{
    uint8_t portb_saved = PIA.portb;
    const uint8_t *src = (const uint8_t*)0x4000u + bank_offset;

    __asm__("sei");
    PIA.portb = bank_portb;
    while (len > 0u) {
        *dst++ = *src++;
        --len;
    }
    PIA.portb = portb_saved;
    __asm__("cli");
}

void bank_copy_to_window(uint16_t bank_offset, const uint8_t *src, uint16_t len, uint8_t bank_portb)
{
    uint8_t portb_saved = PIA.portb;
    uint8_t *dst = (uint8_t*)0x4000u + bank_offset;

    __asm__("sei");
    PIA.portb = bank_portb;
    while (len > 0u) {
        *dst++ = *src++;
        --len;
    }
    PIA.portb = portb_saved;
    __asm__("cli");
}
#pragma code-name(pop)
