#include <stdio.h>
#include <stdint.h>
#include <atari.h>

#include "modplayer.h"
#include "mod_app.h"

extern void vbi_install(void);
extern void vbi_remove(void);

#define MOD_KEY_NONE  255
#define MOD_KEY_SPACE  32

static uint8_t key_read_clear(void)
{
    uint8_t k;
    k = PEEK(0x02FC);
    POKE(0x02FC, MOD_KEY_NONE);
    return k;
}

static void wait_vbi(void)
{
    uint8_t t;
    t = PEEK(RTCLOK);
    while (PEEK(RTCLOK) == t) ;
}

static void display_status(void)
{
    gotoxy(0,22);
    printf("Ord:%3d/%3d Row:%2d BPM:%3d Spd:%d  \r",
           (int)(mod_get_order() + 1),
           (int)mod.song_length,
           (int)mod_get_row(),
           (int)mod.bpm,
           (int)mod.speed);
}

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

void app_player_run(void)
{
    uint8_t key;
    uint8_t paused = 0;

    POKE(0x02FC, MOD_KEY_NONE);

    vbi_install();
    app_player_start();

    while (mod.playing) {
        display_status();

        (void)app_player_main_service();

        key = key_read_clear();
        if (key != MOD_KEY_NONE) {
            if (key == MOD_KEY_SPACE) {
                paused = (uint8_t)!paused;
                mod_pause();
                if (paused) printf("\nPAUSED\n");
                else        printf("\n");
            } else {
                break;
            }
        }

        wait_vbi();
    }

    app_player_stop(1u);
    vbi_remove();

    printf("\nStopped.\n");
}
