#include <stdint.h>

#include "modplayer.h"
#include "mod_app.h"
#include "mod_struct.h"

void app_player_vbi_tick(void)
{
    mod_vbi_tick();
}

void app_player_start(void)
{
    mod_play();
}

void app_player_stop()
{
    mod_stop();
}
