#ifndef POKEYMAX_H
#define POKEYMAX_H
#include <stdint.h>

/* Linux shim: route PEEK/POKE to mock functions instead of raw MMIO */
uint8_t pokeymax_mock_peek(uint16_t addr);
void    pokeymax_mock_poke(uint16_t addr, uint8_t val);

#define POKE(addr, val)   pokeymax_mock_poke((uint16_t)(addr), (uint8_t)(val))
#define PEEK(addr)        pokeymax_mock_peek((uint16_t)(addr))
#define POKE_OR(addr, val)  POKE((addr), (uint8_t)(PEEK(addr) | (val)))
#define POKE_AND(addr, val) POKE((addr), (uint8_t)(PEEK(addr) & (val)))

/* Same register defs as include/pokeymax.h */
#define REG_RAMADDRL    0xD284U
#define REG_RAMADDRH    0xD285U
#define REG_RAMDATA     0xD286U
#define REG_RAMDATAINC  0xD287U
#define REG_CHANSEL     0xD288U
#define REG_ADDRL       0xD289U
#define REG_ADDRH       0xD28AU
#define REG_LENL        0xD28BU
#define REG_LENH        0xD28CU
#define REG_PERL        0xD28DU
#define REG_PERH        0xD28EU
#define REG_VOL         0xD28FU
#define REG_DMA         0xD290U
#define REG_IRQEN       0xD291U
#define REG_IRQACT      0xD292U
#define REG_SAMCFG      0xD293U

#define SAMCFG_8BIT_ALL  0xF0
#define SAMCFG_ADPCM_ALL 0x0F
#define SAMCFG_ADPCM_CH1   0x01
#define SAMCFG_ADPCM_CH2   0x02
#define SAMCFG_ADPCM_CH3   0x04
#define SAMCFG_ADPCM_CH4   0x08
#define SAMCFG_8BIT_CH1    0x10
#define SAMCFG_8BIT_CH2    0x20
#define SAMCFG_8BIT_CH3    0x40
#define SAMCFG_8BIT_CH4    0x80
#define SAM_CH1_BIT     0x01
#define SAM_CH2_BIT     0x02
#define SAM_CH3_BIT     0x04
#define SAM_CH4_BIT     0x08

#define REG_COVOX_CH1   0xD280U
#define REG_COVOX_CH2   0xD281U
#define REG_COVOX_CH3   0xD282U
#define REG_COVOX_CH4   0xD283U

#define REG_CFGUNLOCK   0xD20CU
#define REG_CFGID       0xD21CU
#define REG_CFG_MODE    0xD210U
#define REG_CFG_CAP     0xD211U
#define REG_CFG_RESTRICT 0xD217U

#define CAP_POKEY_MASK  0x03
#define CAP_SID         0x04
#define CAP_PSG         0x08
#define CAP_COVOX       0x10
#define CAP_SAMPLE      0x20
#define CAP_FLASH       0x40

#define RTCLOK      0x0014U
#define CH          0x02FCU

#endif
