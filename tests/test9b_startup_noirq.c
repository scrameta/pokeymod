/*
 * test9b_startup_noirq.c - mod startup path with NO sample IRQ enable
 *
 * Purpose:
 *   Isolate mod_play()/first-row logic from IRQ timing by cloning the mod_play
 *   state reset but intentionally not enabling sample IRQs. Then manually call
 *   mod_vbi_tick() and dump state.
 */

#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include <atari.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

static void print_regs(const char *tag)
{
    printf("%s DMA=%02X IRQEN=%02X IRQACT=%02X SAMCFG=%02X\n",
           tag,
           (unsigned)PEEK(REG_DMA),
           (unsigned)PEEK(REG_IRQEN),
           (unsigned)PEEK(REG_IRQACT),
           (unsigned)PEEK(REG_SAMCFG));
}

static void mod_play_noirq_clone(void)
{
    uint8_t ch;
    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        mod.chan[ch].active     = 0;
        mod.chan[ch].sample_num = 0;
        mod.chan[ch].period     = 0;
        mod.chan[ch].volume     = 0;
        mod.chan[ch].hw_vol     = 0;
        mod.chan[ch].vib_pos    = 0;
    }

    mod.playing   = 1;
    mod.order_pos = 0;
    mod.row       = 0;
    mod.tick      = 0;

    /* mirror safe startup hygiene, but KEEP IRQs OFF */
    POKE(REG_IRQACT, 0x00);
    POKE(REG_IRQEN,  0x00);
}

static void dump_state(const char *tag)
{
    uint8_t ch;
    printf("%s ord=%u row=%u tick=%u play=%u needpf=%u spd=%u bpm=%u\n",
           tag,
           (unsigned)mod.order_pos,
           (unsigned)mod.row,
           (unsigned)mod.tick,
           (unsigned)mod.playing,
           (unsigned)mod_need_prefetch,
           (unsigned)mod.speed,
           (unsigned)mod.bpm);
    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        ChanState *cs = &mod.chan[ch];
        printf("  C%u act=%u smp=%u per=%u vol=%u ad=%u lp=%u addr=%u len=%u\n",
               (unsigned)(ch + 1u),
               (unsigned)cs->active,
               (unsigned)cs->sample_num,
               (unsigned)cs->period,
               (unsigned)cs->volume,
               (unsigned)cs->is_adpcm,
               (unsigned)cs->has_loop,
               (unsigned)cs->sam_addr,
               (unsigned)cs->sam_len);
    }
}

int main(int argc, char **argv)
{
    const char *filename = (argc > 1) ? argv[1] : "D1:MOD.DAT";
    uint8_t i;

    clrscr();
    printf("Test 9b: startup path no IRQ enable\n");
    printf("--------------------------------\n");

    if (PEEK(REG_CFGID) != 1u) { printf("No PokeyMAX\n"); return 1; }
    POKE(REG_CFGUNLOCK, 0x3F);
    if (!(PEEK(REG_CFG_CAP) & CAP_SAMPLE)) {
        POKE(REG_CFGUNLOCK, 0x00);
        printf("No sample player\n");
        return 1;
    }
    POKE(REG_CFGUNLOCK, 0x00);

    printf("Loading %s...\n", filename);
    if (mod_load(filename) != 0u) {
        printf("mod_load FAILED\n");
        return 1;
    }

    print_regs("After load:");
    dump_state("Before play clone:");

    printf("Calling mod_play_noirq_clone()...\n");
    mod_play_noirq_clone();
    print_regs("After play clone:");
    dump_state("After play clone:");

    for (i = 0; i < 8u; i++) {
        printf("mod_vbi_tick #%u...\n", (unsigned)i);
        mod_vbi_tick();
        print_regs("  regs:");
        dump_state("  state:");
        if (!mod.playing) break;
    }

    printf("\nDone. Press key.\n");
    cgetc();
    mod_stop();
    mod_file_close();
    return 0;
}
