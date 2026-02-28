/* test11_row0_decode.c - inspect actual row0 as seen by modplayer cache */
#include <stdio.h>
#include <stdint.h>
#include <conio.h>
#include "modplayer.h"
#include "mod_format.h"

static void dump_row0(void)
{
    uint8_t ch;
    uint8_t pat = mod.order_table[0];
    if (mod_read_row(pat, 0) != 0u) {
        printf("mod_read_row failed (pat=%u)\n", pat);
        return;
    }
    printf("Order0 pat=%u row0 bytes:\n", pat);
    for (ch=0; ch<MOD_CHANNELS; ch++) {
        MODNote raw; Note n;
        raw.raw[0]=mod_row_buf[ch*4u+0u]; raw.raw[1]=mod_row_buf[ch*4u+1u];
        raw.raw[2]=mod_row_buf[ch*4u+2u]; raw.raw[3]=mod_row_buf[ch*4u+3u];
        mod_decode_note(&raw,&n);
        printf("Ch%u raw=%02X %02X %02X %02X  s=%u p=%u fx=%X %02X",
               (unsigned)(ch+1u),raw.raw[0],raw.raw[1],raw.raw[2],raw.raw[3],
               n.sample,n.period,n.effect,n.param);
        if (n.sample>MOD_MAX_SAMPLES) printf("  <-- INVALID SAMPLE");
        else if (n.sample && mod.samples[n.sample].length==0) printf("  <-- EMPTY SAMPLE");
        if (n.period && (n.period<113u || n.period>856u)) printf("  <-- ODD PERIOD");
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *filename = (argc>1)?argv[1]:"D1:MOD.DAT";
    clrscr();
    printf("Test11: Row0 Decode\n------------------\n");
    if (mod_load(filename) != 0u) { printf("Load failed\n"); goto done; }
    printf("Loaded. songlen=%u patterns=%u\n", mod.song_length, mod.num_patterns);
    dump_row0();
    puts("Press key.");
done:
    while (!kbhit()) {}
    mod_stop(); mod_file_close();
    return 0;
}
