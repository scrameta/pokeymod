/*
 * test8_irq_during_foreground.c - Stress cc65 reentrancy from IRQ into foreground C
 *
 * Purpose:
 *   Reproduce crashes caused by calling a cc65 C function from the IRQ hook while
 *   the foreground code is already executing C (main loop / stdio / memcpy / utoa).
 *
 * Why this matters:
 *   test7 proved IRQ-during-VBI was one problem (fixed by split ZP saves + SEI in VBI),
 *   but the full mod player still crashes because the IRQ handler still calls C and can
 *   interrupt normal foreground C code in main(). cc65 runtime/C stack is not reentrant.
 *
 * Strategy:
 *   - Install src/vbi_handler.s hooks (same as player)
 *   - Keep VBI handler tiny (so this is not the VBI overlap bug)
 *   - Generate frequent sample IRQs on channel 1
 *   - IRQ handler is a C function (called from IRQ hook) that ACKs + sets a flag
 *   - Foreground repeatedly does "heavy" C work (memcpy/utoa/stdio) while retriggering
 *
 * Expected:
 *   - With current design (IRQ calls C): likely hang/crash/corruption after some time
 *   - After redesign to no C in IRQ (asm ACK+flag only): stable
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <conio.h>
#include <atari.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"

extern void vbi_install(void);
extern void vbi_remove(void);

static volatile uint16_t irq_count = 0;
static volatile uint16_t vbi_count = 0;
static volatile uint8_t  irq_pending = 0;

static uint8_t work_a[256];
static uint8_t work_b[256];

static const int8_t tiny_sample[16] = {
     0,  40,  80, 120, 127,  90,  50,  10,
     0, -40, -80,-120,-127, -90, -50, -10
};

/* Required by src/vbi_handler.s */
void mod_vbi_tick(void)
{
    ++vbi_count; /* deliberately tiny; this test targets foreground-vs-IRQ C reentry */
}

/* Called from IRQ hook in src/vbi_handler.s (THIS is the thing we suspect is unsafe) */
void pokeymax_loop_handler(void)
{
    ++irq_count;
    POKE(REG_IRQACT, 0x00);   /* ACK all sample IRQ flags immediately */
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

/* Burn time in foreground *using C library/runtime state* to widen IRQ->C collision window */
static void foreground_c_burn(uint16_t iter)
{
    uint16_t i;
    char num[8];

    for (i = 0; i < 256; ++i) {
        work_a[i] = (uint8_t)(i + (uint8_t)iter);
    }
    memcpy(work_b, work_a, sizeof(work_a));

    /* utoa + cputs exercise more cc65 runtime state than raw POKE loops */
    utoa((unsigned)(iter ^ irq_count ^ vbi_count), num, 10);
    gotoxy(0, 11);
    cputs("FG:         ");
    gotoxy(4, 11);
    cputs(num);
}

int main(void)
{
    char irq_buf[6], vbi_buf[6];
    uint16_t last_irq = 0;
    uint16_t iter = 0;

    clrscr();
    printf("Test 8: IRQ during foreground C\n");
    printf("------------------------------\n");

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

    printf("Stress running: fast IRQ + heavy foreground C\n");
    printf("If IRQ->C reentry is unsafe, this may crash/hang.\n");
    printf("Press any key to stop.\n\n");

    POKE(REG_IRQACT, 0x00);
    pokeymax_channel_setup(1, 0, (uint16_t)sizeof(tiny_sample), 300, 40, 1, 0);

    POKE(CH, 255);
    while (PEEK(CH) == 255) {
        uint16_t now;

        /* Keep sample IRQ stream going (foreground retrigger) */
        if (irq_pending) {
            irq_pending = 0;
            pokeymax_channel_trigger(1, 0, (uint16_t)sizeof(tiny_sample));
        }

        /* Heavy foreground C work to increase collision probability */
        foreground_c_burn(iter++);

        now = irq_count;
        if (now != last_irq) {
            gotoxy(0, 9);
            cputs("IRQ:       VBI:      ");
            utoa((unsigned)now, irq_buf, 10);
            utoa((unsigned)vbi_count, vbi_buf, 10);
            gotoxy(5, 9);  cputs(irq_buf);
            gotoxy(16, 9); cputs(vbi_buf);

            if ((now & 0x1F) == 0u) {
                POKE(0xD01A, (uint8_t)((PEEK(0xD01A) + 1) & 0x0F));
            }
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
