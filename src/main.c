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

static uint8_t parse_u8(const char *s, uint8_t *out)
{
    char *end;
    unsigned long v;

    if (!s || !*s) return 1u;
    v = strtoul(s, &end, 0);
    if (*end != '\0' || v > 255ul) return 1u;
    *out = (uint8_t)v;
    return 0u;
}

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t bank_first = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_FIRST;
    uint8_t bank_count = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_COUNT;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pattern-bank-range") == 0) {
            if (i + 2 >= argc ||
                parse_u8(argv[i + 1], &bank_first) != 0u ||
                parse_u8(argv[i + 2], &bank_count) != 0u) {
                return 1;
            }
            i += 2;
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
