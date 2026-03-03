#ifndef BANK_H
#define BANK_H
void bank_copy_from_window(uint8_t *dst, uint16_t bank_offset, uint8_t bank_portb);
void bank_copy_to_window(uint16_t bank_offset, const uint8_t *src, uint16_t len, uint8_t bank_portb);

#endif /* BANK_H */
