/*
 * pokeymax.h - PokeyMAX v1.26 hardware register definitions
 *
 * cc65 does not reliably honour 'volatile' for hardware registers.
 * We use PEEK/POKE macros (plain pointer casts as separate statements)
 * which cc65 always compiles to single lda/sta instructions with no
 * reordering.  Never chain these in a single expression.
 */

#ifndef POKEYMAX_H
#define POKEYMAX_H

#include <stdint.h>

/* -------------------------------------------------------
 * Safe single-cycle hardware register access for cc65
 * ------------------------------------------------------- */
#define POKE(addr, val)   (*(unsigned char *)(addr) = (unsigned char)(val))
#define PEEK(addr)        (*(unsigned char *)(addr))

/* Read-modify-write helper: avoids cc65 volatile problems */
#define POKE_OR(addr, val)  POKE((addr), (unsigned char)(PEEK(addr) | (val)))
#define POKE_AND(addr, val) POKE((addr), (unsigned char)(PEEK(addr) & (val)))

/* -------------------------------------------------------
 * SAMPLE player register addresses  $D284-$D293
 * ------------------------------------------------------- */
#define REG_RAMADDRL    0xD284U
#define REG_RAMADDRH    0xD285U
#define REG_RAMDATA     0xD286U
#define REG_RAMDATAINC  0xD287U  /* write + auto-increment */

#define REG_CHANSEL     0xD288U  /* write channel number 1-4 */
#define REG_ADDRL       0xD289U  /* sample start address lo (buffered) */
#define REG_ADDRH       0xD28AU  /* sample start address hi */
#define REG_LENL        0xD28BU  /* sample length lo (length-1) */
#define REG_LENH        0xD28CU  /* sample length hi */
#define REG_PERL        0xD28DU  /* period lo (immediate) */
#define REG_PERH        0xD28EU  /* period hi */
#define REG_VOL         0xD28FU  /* volume 0-63 (immediate) */
#define REG_DMA         0xD290U  /* DMA enable: bit0=ch1 ... bit3=ch4 */
#define REG_IRQEN       0xD291U  /* IRQ enable per channel */
#define REG_IRQACT      0xD292U  /* IRQ active (read); write 0=clear all */

/* REG_SAMCFG: bits[7:4]=bits8 (1=8bit, resets to 1111), bits[3:0]=adpcm (1=ADPCM) */
#define REG_SAMCFG       0xD293U
#define SAMCFG_8BIT_ALL  0xF0
#define SAMCFG_ADPCM_ALL 0x0F

/* -------------------------------------------------------
 * COVOX manual volume $D280-$D283
 * ------------------------------------------------------- */
#define REG_COVOX_CH1   0xD280U
#define REG_COVOX_CH2   0xD281U
#define REG_COVOX_CH3   0xD282U
#define REG_COVOX_CH4   0xD283U

/* -------------------------------------------------------
 * Config registers
 * Detect: PEEK(REG_CFGID) == 1 means PokeyMAX present.
 * Unlock: POKE(REG_CFGUNLOCK, 0x3F) then access $D210-$D21F.
 * Relock: POKE(REG_CFGUNLOCK, 0x00)
 * ------------------------------------------------------- */
#define REG_CFGUNLOCK   0xD20CU  /* write 0x3F=unlock, 0x00=lock */
#define REG_CFGID       0xD21CU  /* read: 1 = PokeyMAX present */
#define REG_CFG_MODE    0xD210U  /* misc mode register */
#define REG_CFG_CAP     0xD211U  /* capability flags (read-only) */
#define REG_CFG_RESTRICT 0xD217U /* software chip restrict */

/* CAP bits */
#define CAP_POKEY_MASK  0x03
#define CAP_SID         0x04
#define CAP_PSG         0x08
#define CAP_COVOX       0x10
#define CAP_SAMPLE      0x20
#define CAP_FLASH       0x40

/* REG_SAMCFG bit helpers */
#define SAMCFG_ADPCM_CH1   0x01
#define SAMCFG_ADPCM_CH2   0x02
#define SAMCFG_ADPCM_CH3   0x04
#define SAMCFG_ADPCM_CH4   0x08
#define SAMCFG_8BIT_CH1    0x10
#define SAMCFG_8BIT_CH2    0x20
#define SAMCFG_8BIT_CH3    0x40
#define SAMCFG_8BIT_CH4    0x80

/* REG_DMA / REG_IRQEN channel bit masks */
#define SAM_CH1_BIT     0x01
#define SAM_CH2_BIT     0x02
#define SAM_CH3_BIT     0x04
#define SAM_CH4_BIT     0x08

/* -------------------------------------------------------
 * Atari OS addresses used by the player
 * ------------------------------------------------------- */
#define RTCLOK      0x0014U  /* VBI frame counter (increments each VBI) */
#define CH          0x02FCU  /* last key pressed (ATASCII), 255=none */

#endif /* POKEYMAX_H */
