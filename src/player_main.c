#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mod_app.h"
#include "mod_struct.h"
#include "pokeymax.h"

uint8_t app_player_main_service()
{
	//load_pattern();
	return 0;
}

int main(int argc, char *argv[])
{
    printf("\nfilename:%s\n",mod.filename);
    app_player_run();
    return 0;
}
