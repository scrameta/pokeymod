/*
 * test10_main_startup_markers.c - Minimal main-like startup trace
 *
 * Installs the real VBI/IRQ hooks, calls mod_play(), then waits for a couple of
 * VBIs while printing marker lines so we can see roughly where startup hangs.
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

extern void vbi_install(void);
extern void vbi_remove(void);

static void wait_vbi_local(void)
{
    uint8_t t = PEEK(RTCLOK);
    while (PEEK(RTCLOK) == t) ;
}

static void print_regs(const char *tag)
{
    printf("%s DMA=%02X IRQEN=%02X IRQACT=%02X SAMCFG=%02X\n",
           tag,
           (unsigned)PEEK(REG_DMA),
           (unsigned)PEEK(REG_IRQEN),
           (unsigned)PEEK(REG_IRQACT),
           (unsigned)PEEK(REG_SAMCFG));
}

int main(int argc, char **argv)
{
    const char *filename = (argc > 1) ? argv[1] : "D1:MOD.DAT";
    uint8_t i;

    clrscr();
    printf("Test 10: main startup markers\n");
    printf("-----------------------------\n");

    if (PEEK(REG_CFGID) != 1u) { printf("No PokeyMAX\n"); return 1; }
    POKE(REG_CFGUNLOCK, 0x3F);
    if (!(PEEK(REG_CFG_CAP) & CAP_SAMPLE)) {
        POKE(REG_CFGUNLOCK, 0x00);
        printf("No sample player\n");
        return 1;
    }
    POKE(REG_CFGUNLOCK, 0x00);

    printf("[1] load %s\n", filename);
    if (mod_load(filename) != 0u) {
        printf("mod_load FAILED\n");
        return 1;
    }
    print_regs(" after load:");

    printf("[2] vbi_install\n");
    vbi_install();
    print_regs(" after install:");

    printf("[3] mod_play\n");
    mod_play();
    print_regs(" after play:");
    printf(" state ord=%u row=%u tick=%u play=%u\n",
           (unsigned)mod.order_pos,
           (unsigned)mod.row,
           (unsigned)mod.tick,
           (unsigned)mod.playing);

    for (i = 0; i < 4u; i++) {
        printf("[4.%u] wait_vbi...\n", (unsigned)i);
        wait_vbi_local();
        print_regs("  regs:");
        printf("  state ord=%u row=%u tick=%u play=%u needpf=%u\n",
               (unsigned)mod.order_pos,
               (unsigned)mod.row,
               (unsigned)mod.tick,
               (unsigned)mod.playing,
               (unsigned)mod_need_prefetch);
    }

    printf("[5] stop/remove\n");
    mod_stop();
    vbi_remove();
    mod_file_close();
    print_regs(" after stop:");

    printf("Press key.\n");
    cgetc();
    return 0;
}
