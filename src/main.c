#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

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

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static uint8_t parse_bank_spec(const char *s, uint8_t *first, uint8_t *count)
{
    const char *sep;
    char *end;

    if (!s || !*s) return 1u;

    sep = strchr(s, ',');
    if (!sep) sep = strchr(s, ':');
    if (sep) {
        unsigned long a;
        unsigned long b;
        a = strtoul(s, &end, 16);
        if (end != sep || a > 255ul) return 1u;
        b = strtoul(sep + 1, &end, 16);
        if (*end != '\0' || b > 255ul) return 1u;
        *first = (uint8_t)a;
        *count = (uint8_t)b;
        return 0u;
    }

    if (s[0] && s[1] && s[2] == '\0') {
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if (hi >= 0 && lo >= 0) {
            *first = (uint8_t)hi;
            *count = (uint8_t)lo;
            return 0u;
        }
    }

    return parse_u8(s, count);
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
                if (parse_bank_spec(argv[i + 1], &bank_first, &bank_count) != 0u) {
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
            if (parse_bank_spec(argv[i] + 4, &bank_first, &bank_count) != 0u) {
                return 1;
            }
            continue;
        }

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
