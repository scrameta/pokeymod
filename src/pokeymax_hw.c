/*
 * pokeymax_hw.c - PokeyMAX sample player hardware driver
 *
 * Key facts from VHDL (sample/top.vhdl, sample/channel.vhdl):
 *
 * TRIGGER: Any toggle of a channel's DMA bit causes syncreset, which
 *   resets the channel to start_addr on the next clock. To start a
 *   fresh sample: ensure DMA bit is 0, write all regs, then set DMA=1.
 *   To retrigger mid-play: toggle 1->0->1.
 *
 * SIGN BIT INVERSION: top.vhdl line:
 *   store_data(12) <= not(ram_data(7))
 *   The hardware inverts bit 7 of every 8-bit sample when storing into
 *   its internal audio buffer. This means PCM data must be written with
 *   bit 7 pre-inverted so the hardware un-inverts it correctly.
 *   Original signed PCM: -128..+127 (bit7=sign, two's complement)
 *   We must XOR each byte with 0x80 before writing to block RAM.
 *   This converts: silence(0) -> 0x80, +127 -> 0x7F, -128 -> 0x00
 *   Which after hardware inversion becomes: silence(0x80->0), etc. -- correct.
 *
 * REG_SAMCFG ($D293):
 *   bits [7:4] = bits8_reg (1=8-bit, resets to 1111)
 *   bits [3:0] = adpcm_reg (1=ADPCM)
 *   For 8-bit PCM: write 0xF0. NEVER write 0x00 (clears bits8 -> 4-bit mode).
 *
 * PERIOD: 12-bit. hw_period = amiga_period / 2.
 *   (PokeyMAX ENABLE=PHI2~1.77MHz, Paula=3.55MHz, so half the period.)
 *
 * CHANSEL: write channel number 1-4 before addr/len/period/vol writes.
 *
 * IRQ ACT ($D292): write 0x00 to clear all, or write mask of bits to KEEP.
 */

#include <stdint.h>
#include "pokeymax.h"
#include "pokeymax_hw.h"

uint16_t pokeymax_ram_ptr = 0;
static uint8_t pokeymax_samcfg_shadow = 0xF0;

void pokeymax_init(void)
{
    pokeymax_ram_ptr = 0;
    POKE(REG_DMA,    0x00);
    POKE(REG_IRQEN,  0x00);
    POKE(REG_IRQACT, 0x00);
    /* bits8=1111 (8-bit mode), adpcm=0000 */
    pokeymax_samcfg_shadow = 0xF0;
    POKE(REG_SAMCFG, pokeymax_samcfg_shadow);
    POKE(REG_COVOX_CH1, 128);
    POKE(REG_COVOX_CH2, 128);
    POKE(REG_COVOX_CH3, 128);
    POKE(REG_COVOX_CH4, 128);
}

uint16_t pokeymax_alloc(uint16_t bytes)
{
    uint16_t addr;
    if ((uint32_t)pokeymax_ram_ptr + bytes > (uint32_t)POKEYMAX_RAM_SIZE)
        return POKEYMAX_ALLOC_FAIL;
    addr = pokeymax_ram_ptr;
    pokeymax_ram_ptr += bytes;
    return addr;
}

/*
 * pokeymax_write_ram() - write bytes to PokeyMAX block RAM.
 * Samples are 8-bit signed per documentation. No bit manipulation needed.
 */
void pokeymax_write_ram(uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    POKE(REG_RAMADDRL, (unsigned char)(addr & 0xFF));
    POKE(REG_RAMADDRH, (unsigned char)(addr >> 8));
    for (i = 0; i < len; i++)
        POKE(REG_RAMDATAINC, data[i]);
}

void pokeymax_channel_setup(uint8_t chan, uint16_t addr, uint16_t len,
                             uint16_t period, uint8_t vol,
                             uint8_t mode_8bit, uint8_t mode_adpcm)
{
    unsigned char bit  = (unsigned char)(1u << (chan - 1u));
    unsigned char nbit = (unsigned char)(~bit & 0x0Fu);
    unsigned char dma, cfg, irq;

    POKE(REG_CHANSEL, chan);
    POKE(REG_ADDRL, (unsigned char)(addr & 0xFF));
    POKE(REG_ADDRH, (unsigned char)(addr >> 8));
    POKE(REG_LENL,  (unsigned char)((len-1u) & 0xFF));  /* doc: Length=L+H*256+1 */
    POKE(REG_LENH,  (unsigned char)((len-1u) >> 8));
    POKE(REG_PERL,  (unsigned char)(period & 0xFF));
    POKE(REG_PERH,  (unsigned char)((period >> 8) & 0x0Fu));
    POKE(REG_VOL,   (unsigned char)(vol & 0x3Fu));

    /* Update SAMCFG for this channel only (preserve other channels):
     * low nibble  bit(ch-1) = ADPCM enable for channel
     * high nibble bit(ch+3) = 8-bit mode for channel
     */
    /*
     * REG_SAMCFG is write-only on PokeyMAX RTL (no readback path in DO mux).
     * Reading it works in the Linux shim but returns undefined data on hardware,
     * which can randomly flip channels between 8-bit/4-bit/ADPCM modes.
     * Keep a software shadow and write that out instead of PEEK(REG_SAMCFG).
     */
    cfg = pokeymax_samcfg_shadow;
    cfg = (unsigned char)(cfg & (unsigned char)~bit);                  /* clear ADPCM bit */
    cfg = (unsigned char)(cfg & (unsigned char)~(unsigned char)(bit << 4u)); /* clear 8-bit bit */
    if (mode_adpcm) {
        cfg = (unsigned char)(cfg | bit);                              /* ADPCM=1, 8-bit=0 */
    } else {
        cfg = (unsigned char)(cfg | (unsigned char)(bit << 4u));       /* ADPCM=0, 8-bit=1 */
    }
    pokeymax_samcfg_shadow = cfg;
    POKE(REG_SAMCFG, pokeymax_samcfg_shadow);

    /* Enable IRQ for this channel */
    irq = PEEK(REG_IRQEN);
    POKE(REG_IRQEN, (unsigned char)(irq | bit));

    /* Ensure DMA is off, then turn on -> clean 0->1 trigger */
    dma = PEEK(REG_DMA);
    if (dma & bit) {
        POKE(REG_DMA, (unsigned char)(dma & nbit));
    }
    dma = PEEK(REG_DMA);
    POKE(REG_DMA, (unsigned char)(dma | bit));

    (void)mode_8bit;
}

/*
 * pokeymax_channel_trigger() - retrigger with new addr/len (for loops)
 * Toggles DMA 1->0->1 to restart from new address.
 */
void pokeymax_channel_trigger(uint8_t chan, uint16_t addr, uint16_t len)
{
    unsigned char bit  = (unsigned char)(1u << (chan - 1u));
    unsigned char nbit = (unsigned char)(~bit & 0x0Fu);
    unsigned char dma;

    POKE(REG_CHANSEL, chan);
    POKE(REG_ADDRL, (unsigned char)(addr & 0xFF));
    POKE(REG_ADDRH, (unsigned char)(addr >> 8));
    POKE(REG_LENL,  (unsigned char)((len-1u) & 0xFF));  /* doc: Length=L+H*256+1 */
    POKE(REG_LENH,  (unsigned char)((len-1u) >> 8));

    dma = PEEK(REG_DMA);
    POKE(REG_DMA, (unsigned char)(dma & nbit));
    dma = PEEK(REG_DMA);
    POKE(REG_DMA, (unsigned char)(dma | bit));
}

void pokeymax_channel_set_period_vol(uint8_t chan, uint16_t period, uint8_t vol)
{
    POKE(REG_CHANSEL, chan);
    POKE(REG_PERL, (unsigned char)(period & 0xFF));
    POKE(REG_PERH, (unsigned char)((period >> 8) & 0x0Fu));
    POKE(REG_VOL,  (unsigned char)(vol & 0x3Fu));
}

void pokeymax_channel_stop(uint8_t chan)
{
    unsigned char bit  = (unsigned char)(1u << (chan - 1u));
    unsigned char nbit = (unsigned char)(~bit & 0x0Fu);
    unsigned char dma  = PEEK(REG_DMA);
    POKE(REG_DMA,   (unsigned char)(dma & nbit));
    {
        unsigned char irq = PEEK(REG_IRQEN);
        POKE(REG_IRQEN, (unsigned char)(irq & nbit));
    }
}

void pokeymax_channel_dma_on(uint8_t chan)
{
    unsigned char bit = (unsigned char)(1u << (chan - 1u));
    unsigned char dma = PEEK(REG_DMA);
    if (!(dma & bit))
        POKE(REG_DMA, (unsigned char)(dma | bit));
}

void pokeymax_irq_enable_all(void)
{
    POKE(REG_IRQEN, 0x0Fu);
}
