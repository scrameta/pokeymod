#include <stdint.h>

#include "mod_app.h"

int main(int argc, char *argv[])
{
    const char *filename = (argc > 1) ? argv[1] : "D1:MOD.DAT";

    if (app_loader_run(filename, 1u) != 0u) {
        return 1;
    }

    app_player_run();
    return 0;
}

