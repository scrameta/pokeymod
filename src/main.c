#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "mod_app.h"
#include "modplayer.h"

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_FIRST
#define MODPLAY_DEFAULT_PATTERN_BANK_FIRST 0u
#endif

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_COUNT
#define MODPLAY_DEFAULT_PATTERN_BANK_COUNT 0u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_FIRST
#define MODPLAY_AUTO_PATTERN_BANK_FIRST 4u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_COUNT
#define MODPLAY_AUTO_PATTERN_BANK_COUNT 8u
#endif

static uint8_t parse_bank_bits_hex(const char *s, uint8_t *first, uint8_t *count)
{
    uint8_t bits;
    uint8_t first_bit;
    uint8_t bit_count;
    char *end;
    unsigned long v;

    if (!s || !*s) return 1u;
    v = strtoul(s, &end, 16);
    if (*end != '\0' || v > 255ul) return 1u;

    bits = (uint8_t)v;
    if (bits == 0u) {
        *first = 0u;
        *count = 0u;
        return 0u;
    }

    first_bit = 0u;
    while (((bits >> first_bit) & 1u) == 0u) {
        first_bit++;
    }

    bit_count = 0u;
    while (bits) {
        bit_count += (uint8_t)(bits & 1u);
        bits >>= 1;
    }

    *first = (uint8_t)(1u << first_bit);
    *count = (uint8_t)(1u << (bit_count - 1u));
    return 0u;
}

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t bank_first = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_FIRST;
    uint8_t bank_count = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_COUNT;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--b") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (parse_bank_bits_hex(argv[i + 1], &bank_first, &bank_count) != 0u) {
                    return 1;
                }
                i += 1;
            } else {
                bank_first = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_FIRST;
                bank_count = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_COUNT;
            }
            continue;
        }

        if (strncmp(argv[i], "--b=", 4) == 0) {
            if (parse_bank_bits_hex(argv[i] + 4, &bank_first, &bank_count) != 0u) {
                return 1;
            }
            continue;
        }

        if (strcmp(argv[i], "--no-pattern-banks") == 0) {
            bank_count = 0u;
            continue;
        }

        filename = argv[i];
    }

    mod_set_pattern_bank_range(bank_first, bank_count);

    if (app_loader_run(filename, 1u) != 0u) {
        return 1;
    }

    app_player_run();
    return 0;
}
