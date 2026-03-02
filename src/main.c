#include <stdint.h>
#include <string.h>

#include "mod_app.h"
#include "modplayer.h"

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_FIRST
#define MODPLAY_DEFAULT_PATTERN_BANK_FIRST 0u
#endif

#ifndef MODPLAY_DEFAULT_PATTERN_BANK_COUNT
#define MODPLAY_DEFAULT_PATTERN_BANK_COUNT 0u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_FIRST
#define MODPLAY_AUTO_PATTERN_BANK_FIRST 0u
#endif

#ifndef MODPLAY_AUTO_PATTERN_BANK_COUNT
#define MODPLAY_AUTO_PATTERN_BANK_COUNT 4u
#endif

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t bank_first = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_FIRST;
    uint8_t bank_count = (uint8_t)MODPLAY_DEFAULT_PATTERN_BANK_COUNT;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--b") == 0) {
            bank_first = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_FIRST;
            bank_count = (uint8_t)MODPLAY_AUTO_PATTERN_BANK_COUNT;
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
