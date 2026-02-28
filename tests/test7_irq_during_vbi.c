/*
 * test7_irq_during_vbi.c - Stress nested IRQ during deferred VBI
 *
 * Purpose:
 *   Reproduce races where the PokeyMAX sample IRQ fires while the player's
 *   deferred VBI handler is in progress. This is the scenario most likely to
 *   corrupt cc65 zero page temporaries if VBI and IRQ share one save buffer.
 *
 * Strategy:
 *   - Install the same src/vbi_handler.s hooks used by the player
 *   - mod_vbi_tick() intentionally burns CPU cycles (long VBI)
 *   - Sample IRQ runs rapidly from a tiny looping sample
 *   - Foreground retriggers sample after IRQ ACK (same pattern as test6)
 *
 * Expected:
 *   - With vulnerable shared-ZP-save handler: may crash/hang/reset after some time
 *   - With fixed split-ZP-save handler: runs stably, counters increase continuously
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>
#include <stdlib.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"

extern void vbi_install(void);
extern void vbi_remove(void);

static volatile uint16_t irq_count = 0;
static volatile uint16_t vbi_count = 0;
static volatile uint16_t nested_hits = 0;
static volatile uint8_t irq_pending = 0;
static volatile uint8_t in_vbi = 0;

/* Tiny waveform so sample-end IRQs happen often. */
static const int8_t tiny_sample[16] = {
     0,  40,  80, 120, 127,  90,  50,  10,
     0, -40, -80,-120,-127, -90, -50, -10
};

/*
 * Required by src/vbi_handler.s
 * Intentionally long to widen the window where IRQ can interrupt us.
 */
void mod_vbi_tick(void)
{
    volatile uint8_t i, j;
    volatile uint8_t sink = 0;

    in_vbi = 1;
    ++vbi_count;

    /* Busy work: pure CPU burn, no OS calls in ISR context. */
    for (j = 0; j < 24; ++j) {
        for (i = 0; i < 180; ++i) {
            sink = (uint8_t)(sink + (uint8_t)(i ^ j));
        }
    }

    /* Prevent over-optimization paranoia (cc65 usually respects volatile). */
    if (sink == 0xFF) {
        POKE(0xD01A, (uint8_t)((PEEK(0xD01A) + 1) & 0x0F));
    }

    in_vbi = 0;
}

/* Called from IRQ hook in src/vbi_handler.s */
void pokeymax_loop_handler(void)
{
    ++irq_count;
    if (in_vbi) {
        ++nested_hits; /* Evidence IRQ landed during deferred VBI */
    }

    /* IRQ context: keep it minimal and deterministic. */
    POKE(REG_IRQACT, 0x00);   /* ACK sample IRQ */
    irq_pending = 1;          /* retrigger in foreground */
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
    char irq_buf[6], vbi_buf[6], nest_buf[6];
    uint16_t last_irq = 0;

    clrscr();
    printf("Test 7: IRQ during long VBI\n");
    printf("---------------------------\n");

    if (!detect_sample_player()) {
        printf("No PokeyMAX sample player found.\n");
        printf("Press key to exit.\n");
        POKE(CH, 255);
        while (PEEK(CH) == 255) ;
        return 1;
    }

    pokeymax_init();
    pokeymax_write_ram(0, (const uint8_t*)tiny_sample, (uint16_t)sizeof(tiny_sample));

    vbi_install();

    printf("Stress running: long VBI + fast sample IRQ\n");
    printf("Nested should increase if overlap occurs.\n");
    printf("If unstable handler, may crash/hang.\n");
    printf("Press any key to stop.\n\n");

    POKE(REG_IRQACT, 0x00);
    /* Small sample + high-ish rate => frequent sample-end IRQs */
    pokeymax_channel_setup(1, 0, (uint16_t)sizeof(tiny_sample), 300, 40, 1, 0);

    POKE(CH, 255);
    while (PEEK(CH) == 255) {
        uint16_t now;

        if (irq_pending) {
            irq_pending = 0;
            pokeymax_channel_trigger(1, 0, (uint16_t)sizeof(tiny_sample));
        }

        now = irq_count;
        if (now != last_irq) {
            gotoxy(0, 10);
            cputs("IRQ:      VBI:      NEST:     ");
            utoa((unsigned)now, irq_buf, 10);
            utoa((unsigned)vbi_count, vbi_buf, 10);
            utoa((unsigned)nested_hits, nest_buf, 10);
            gotoxy(5, 10);  cputs(irq_buf);
            gotoxy(15, 10); cputs(vbi_buf);
            gotoxy(26, 10); cputs(nest_buf);

            if ((now & 0x0F) == 0) {
                POKE(0xD01A, (uint8_t)((PEEK(0xD01A) + 1) & 0x0F));
            }
            last_irq = now;
        }

        waitvsync();
    }

    pokeymax_channel_stop(1);
    POKE(REG_IRQEN, 0x00);
    POKE(REG_IRQACT, 0x00);
    vbi_remove();

    POKE(0xD01A, 0x00);
    POKE(CH, 255);
    printf("\nStopped IRQ=%u VBI=%u NEST=%u\n",
           (unsigned)irq_count, (unsigned)vbi_count, (unsigned)nested_hits);
    return 0;
}
