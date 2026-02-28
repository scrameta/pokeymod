/*
 * test9_startup_path.c - Isolate modplayer startup/first-row path
 *
 * Goal: exercise the real loader + modplayer engine without installing VBI/IRQ.
 * We call mod_play() and then manually call mod_vbi_tick() a few times while
 * printing decoded row 0 and key hardware registers after each step.
 *
 * If this hangs, the problem is in first-row decode/trigger logic itself.
 * If this is stable, the remaining bug is likely in installed interrupt/vector
 * interaction (or timing/race specific to real VBI/IRQ cadence).
 */

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <atari.h>

#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

extern void mod_vbi_tick(void);

static void print_regs(const char *tag)
{
    printf("%s DMA=%02X IRQEN=%02X IRQACT=%02X SAMCFG=%02X\n",
           tag,
           (unsigned)PEEK(REG_DMA),
           (unsigned)PEEK(REG_IRQEN),
           (unsigned)PEEK(REG_IRQACT),
           (unsigned)PEEK(REG_SAMCFG));
}

static void dump_row0_decode(void)
{
    uint8_t pat = mod.order_table[0];
    uint8_t ch;

    if (mod_read_row(pat, 0) != 0u) {
        printf("mod_read_row(pat=%u,row=0) FAILED\n", (unsigned)pat);
        return;
    }

    printf("Row0 pattern %u decode:\n", (unsigned)pat);
    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        MODNote raw;
        Note n;
        raw.raw[0] = mod_row_buf[ch*4u+0u];
        raw.raw[1] = mod_row_buf[ch*4u+1u];
        raw.raw[2] = mod_row_buf[ch*4u+2u];
        raw.raw[3] = mod_row_buf[ch*4u+3u];
        mod_decode_note(&raw, &n);
        printf("  Ch%u smp=%u per=%u fx=%X p=%02X\n",
               (unsigned)(ch+1u), (unsigned)n.sample, (unsigned)n.period,
               (unsigned)n.effect, (unsigned)n.param);
    }
}

static void dump_chan_state(void)
{
    uint8_t ch;
    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        ChanState *cs = &mod.chan[ch];
        printf("  C%u act=%u smp=%u per=%u vol=%u hwv=%u ad=%u lp=%u addr=%u len=%u\n",
               (unsigned)(ch+1u),
               (unsigned)cs->active,
               (unsigned)cs->sample_num,
               (unsigned)cs->period,
               (unsigned)cs->volume,
               (unsigned)cs->hw_vol,
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
    printf("Test 9: mod startup path\n");
    printf("------------------------\n");

    if (PEEK(REG_CFGID) != 1u) {
        printf("No PokeyMAX\n");
        return 1;
    }
    POKE(REG_CFGUNLOCK, 0x3F);
    if (!(PEEK(REG_CFG_CAP) & CAP_SAMPLE)) {
        POKE(REG_CFGUNLOCK, 0x00);
        printf("No sample player\n");
        return 1;
    }
    POKE(REG_CFGUNLOCK, 0x00);
    printf("PokeyMAX sample player OK\n");

    printf("Loading %s...\n", filename);
    if (mod_load(filename) != 0u) {
        printf("mod_load FAILED\n");
        return 1;
    }

    printf("Song len=%u pats=%u RAM=%u/%u\n",
           (unsigned)mod.song_length,
           (unsigned)mod.num_patterns,
           (unsigned)pokeymax_ram_ptr,
           (unsigned)POKEYMAX_RAM_SIZE);
    print_regs("After load:");
    dump_row0_decode();

    printf("\nCalling mod_play() (no VBI/IRQ hooks installed)...\n");
    mod_play();
    print_regs("After play:");
    printf("State: playing=%u ord=%u row=%u tick=%u spd=%u bpm=%u\n",
           (unsigned)mod.playing,
           (unsigned)mod.order_pos,
           (unsigned)mod.row,
           (unsigned)mod.tick,
           (unsigned)mod.speed,
           (unsigned)mod.bpm);
    dump_chan_state();

    printf("\nManual mod_vbi_tick() calls:\n");
    for (i = 0; i < 8u; i++) {
        printf("Tick call %u... ", (unsigned)i);
        mod_vbi_tick();
        printf("ok\n");
        printf("  state: ord=%u row=%u tick=%u playing=%u needpf=%u\n",
               (unsigned)mod.order_pos,
               (unsigned)mod.row,
               (unsigned)mod.tick,
               (unsigned)mod.playing,
               (unsigned)mod_need_prefetch);
        print_regs("  regs:");
        dump_chan_state();
    }

    printf("\nIf this test is stable but modplay hangs, the issue is likely\n");
    printf("in installed interrupt timing/chaining, not first-row decode.\n");
    printf("\nPress key...\n");
    cgetc();
    mod_stop();
    mod_file_close();
    return 0;
}
