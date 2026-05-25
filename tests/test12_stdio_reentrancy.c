#include <atari.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "modplayer.h"

void vbi_install(void);
void vbi_remove(void);

static const int8_t sine64[64] = {
     0, 12, 25, 37, 49, 60, 71, 80, 88, 95,101,105,108,109,108,105,
   101, 95, 88, 80, 71, 60, 49, 37, 25, 12,  0,-12,-25,-37,-49,-60,
   -71,-80,-88,-95,-101,-105,-108,-109,-108,-105,-101,-95,-88,-80,-71,-60,
   -49,-37,-25,-12,  0, 12, 25, 37, 49, 60, 71, 80, 88, 95,101,105
};

static uint8_t pcm[256];

static void build_loop_sample(void){
    unsigned i;
    for(i=0;i<sizeof(pcm);++i){
        int8_t s = sine64[i & 63];
        pcm[i] = (uint8_t)(((uint8_t)s) ^ 0x80u);
    }
}

int main(void){
    FILE *f;
    uint8_t buf[32];
    uint16_t addr;
    unsigned long reads = 0;

    clrscr();
    cputs("test12 stdio+playback stress\r\n");

    pokeymax_init();
    build_loop_sample();
    addr = pokeymax_alloc(sizeof(pcm));
    if(addr==POKEYMAX_ALLOC_FAIL){ cputs("alloc fail\r\n"); return 1; }
    pokeymax_write_ram(addr, pcm, sizeof(pcm));

    /* Configure channel 1 looping sample via mod state so pending-loop service can retrigger it. */
    memset(&mod, 0, sizeof(mod));
    mod.playing = 1;
    mod.chan[0].active = 1;
    mod.chan[0].sam_addr = addr;
    mod.chan[0].sam_len = sizeof(pcm);
    mod.chan[0].has_loop = 1;
    mod.chan[0].loop_start = 0;
    mod.chan[0].loop_len = sizeof(pcm);
    mod.chan[0].is_adpcm = 0;
    mod.chan[0].period = 856;
    mod.chan[0].hw_vol = 48;

    pokeymax_channel_setup(1, addr, sizeof(pcm), 1712, 48, 1, 0);
    pokeymax_channel_dma_on(1);

    vbi_install();
    cputs("Hooks installed. Doing repeated fread/fseek; press key to stop.\r\n");

    f = fopen("D:BLADERUN.MOD", "rb");
    if(!f){ cputs("open D:BLADERUN.MOD failed\r\n"); }

    while(!kbhit()){
        if(f){
            fseek(f, 1084L, SEEK_SET);
            fread(buf,1,sizeof(buf),f);
        }
        ++reads;
        if((reads & 255UL)==0){
            gotoxy(0,5);
            cprintf("reads=%lu irqpend=%u dma=%02X irqen=%02X irqact=%02X   ", reads,
                    (unsigned)pokeymax_irq_pending,
                    (unsigned)PEEK(REG_DMA), (unsigned)PEEK(REG_IRQEN), (unsigned)PEEK(REG_IRQACT));
        }
    }

    if(f) fclose(f);
    vbi_remove();
    POKE(REG_DMA,0); POKE(REG_IRQEN,0); POKE(REG_IRQACT,0);
    cputs("\r\nDone.\r\n");
    return 0;
}
