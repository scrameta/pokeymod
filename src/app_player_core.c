#include <stdint.h>

#include "modplayer.h"
#include "mod_app.h"

void app_player_vbi_tick(void)
{
    mod_vbi_tick();
}

uint8_t app_player_irq_handler(void)
{
    return mod_sample_irq_service();
}

uint8_t app_player_main_service(void)
{
    if (mod_need_prefetch) {
        uint8_t row = mod_get_row();
        if ((row >= 4u && mod.tick != 0u) || row >= 48u) {
            return mod_prefetch_next_pattern();
        }
    }
    return 0u;
}

void app_player_start(void)
{
    mod_play();
}

void app_player_stop(uint8_t close_file)
{
    mod_stop();
    if (close_file) {
        mod_file_close();
    }
}
