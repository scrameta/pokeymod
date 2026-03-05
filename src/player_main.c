#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mod_app.h"
#include "mod_struct.h"

int main(int argc, char *argv[])
{
    printf("\nI am here!");
    printf("\nfilename:%s\n",mod.filename);
    app_player_run();
    return 0;
}
