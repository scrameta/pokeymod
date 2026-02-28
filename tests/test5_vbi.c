/*
 * test5_vbi.c - Verify deferred VBI hook runs reliably
 *
 * This uses the same vbi_handler.s as the player, but replaces
 * mod_vbi_tick() with a tiny counter so we can validate that:
 *   - vbi_install()/vbi_remove() works
 *   - our deferred VBI callback runs every frame
 *   - ISR save/restore does not crash the main loop
 *
 * Expected on Atari:
 *   - vbi_ticks keeps increasing while running
 *   - screen border color changes once per 16 VBIs
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>
#include "pokeymax.h"

static uint16_t read_tick_stable(volatile uint16_t* p)
{
    uint8_t hi1, lo, hi2;
    do {
        hi1 = ((volatile uint8_t*)p)[1];
        lo  = ((volatile uint8_t*)p)[0];
        hi2 = ((volatile uint8_t*)p)[1];
    } while (hi1 != hi2);
    return (uint16_t)(((uint16_t)hi1 << 8) | lo);
}

extern void vbi_install(void);
extern void vbi_remove(void);

static volatile uint16_t vbi_ticks = 0;

/* Required by src/vbi_handler.s */
void mod_vbi_tick(void)
{
    ++vbi_ticks;

    /* Visible heartbeat: cycle border color every 16 ticks */
    if ((vbi_ticks & 0x0F) == 0) {
        POKE(0xD01A, (unsigned char)((PEEK(0xD01A) + 1) & 0x0F));
    }
}

/* Required by src/vbi_handler.s (not used in this test) */
void pokeymax_loop_handler(void)
{
}

int main(void)
{
    uint16_t last = 0;

    clrscr();
    printf("Test 5: Deferred VBI\n");
    printf("--------------------\n");
    printf("Installs player VBI hook only.\n\n");
    printf("Expected: ticks keep increasing.\n");
    printf("Press any key to stop.\n\n");

    POKE(CH, 255);
    vbi_install();

    while (PEEK(CH) == 255) {
        uint16_t now = read_tick_stable(&vbi_ticks);
        if (now != last) {
            gotoxy(0, 7);
            cprintf("VBI ticks: %5u", (unsigned)now);
            last = now;
        }

        /* Keep this in the foreground loop, not ISR context. */
        waitvsync();
    }

    vbi_remove();
    POKE(0xD01A, 0x00);
    POKE(CH, 255);

    printf("\nStopped at %u ticks.\n", (unsigned)read_tick_stable(&vbi_ticks));
    return 0;
}
