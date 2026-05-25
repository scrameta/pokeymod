#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mod_app.h"
#include "mod_struct.h"
#include "mod_pattern_bank_loader.h"
#include "mod_pattern_io.h"

uint8_t app_player_main_service()
{
	//load_pattern();
	return 0;
}

int main(int argc, char *argv[])
{
    const char *filename = "D1:MOD.DAT";
    uint8_t i;

    for (i = 1; i < argc; i++) {
        filename = argv[i];
    }

    if (app_loader_run(filename, 1u) != 0u) {
        return 1;
    }

    load_patterns_to_banks();

    printf("Loading player\n");
    //printf("malloc %x\n",malloc(PAT_BYTES*2));
    printf("\nfilename:%s\n",mod.filename);
    app_player_run();
    return 0;
}
