/*
 * test6_irq.c - Verify PokeyMAX sample IRQ path via player IRQ hook
 *
 * This test installs src/vbi_handler.s IRQ hook and defines a local
 * pokeymax_loop_handler() that increments a counter each IRQ.
 *
 * We configure channel 1 with a tiny 8-bit sample so sample-end IRQs
 * occur quickly. In the loop handler we retrigger the same sample,
 * producing a steady IRQ stream.
 *
 * Expected on Atari:
 *   - irq_count increases continuously
 *   - border color changes regularly
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"

extern void vbi_install(void);
extern void vbi_remove(void);

static volatile uint16_t irq_count = 0;
static volatile uint16_t vbi_count = 0;

static const uint8_t pulse_sample[32] = {
    0, 32, 64, 96, 127, 96, 64, 32,
    0, -32, -64, -96, -127, -96, -64, -32,
    0, 32, 64, 96, 127, 96, 64, 32,
    0, -32, -64, -96, -127, -96, -64, -32
};

/* Required by src/vbi_handler.s */
void mod_vbi_tick(void)
{
    ++vbi_count;
}

/* Called from IRQ hook in src/vbi_handler.s */
void pokeymax_loop_handler(void)
{
    ++irq_count;

    /* Clear all active sample IRQ flags */
    POKE(REG_IRQACT, 0x00);

    /* Keep generating IRQs by retriggering channel 1 */
    pokeymax_channel_trigger(1, 0, (uint16_t)sizeof(pulse_sample));

    /* Visible heartbeat from IRQ context */
    if ((irq_count & 0x1F) == 0) {
        POKE(0xD01A, (unsigned char)((PEEK(0xD01A) + 1) & 0x0F));
    }
}

static uint8_t detect_sample_player(void)
{
    uint8_t cap;
    if (PEEK(REG_CFGID) != 1u) return 0;
    POKE(REG_CFGUNLOCK, 0x3F);
    cap = PEEK(REG_CFG_CAP);
    POKE(REG_CFGUNLOCK, 0x00);
    return (uint8_t)((cap & CAP_SAMPLE) != 0u);
}

int main(void)
{
    uint16_t last_irq = 0;

    clrscr();
    printf("Test 6: PokeyMAX IRQ\n");
    printf("--------------------\n");

    if (!detect_sample_player()) {
        printf("No PokeyMAX sample player found.\n");
        printf("Press key to exit.\n");
        POKE(CH, 255);
        while (PEEK(CH) == 255) ;
        return 1;
    }

    pokeymax_init();
    pokeymax_write_ram(0, pulse_sample, (uint16_t)sizeof(pulse_sample));

    /* Install player VBI+IRQ hooks */
    vbi_install();

    /* Trigger channel 1 once; loop handler retriggers thereafter */
    pokeymax_channel_setup(1, 0, (uint16_t)sizeof(pulse_sample), 428, 40, 1, 0);

    printf("IRQ test running.\n");
    printf("Expected: IRQ count increases.\n");
    printf("Press any key to stop.\n\n");

    POKE(CH, 255);
    while (PEEK(CH) == 255) {
        uint16_t now = irq_count;
        if (now != last_irq) {
            printf("IRQ: %5u  VBI: %5u\r", (unsigned)now, (unsigned)vbi_count);
            last_irq = now;
        }
    }

    pokeymax_channel_stop(1);
    POKE(REG_IRQEN, 0x00);
    POKE(REG_IRQACT, 0x00);
    vbi_remove();

    POKE(0xD01A, 0x00);
    POKE(CH, 255);
    printf("\nStopped IRQ=%u VBI=%u\n", (unsigned)irq_count, (unsigned)vbi_count);
    return 0;
}
