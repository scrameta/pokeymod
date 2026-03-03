/*
 * Linux harness for PokeyMAX MOD player logic + mock sample engine.
 *
 * Goal: make row/order/prefetch + loop/retrigger behavior debuggable on desktop
 * without Atari hardware.
 *
 * Build example (from repo root):
 *   gcc -std=c99 -O0 -g -Wall -Wextra \
 *     -Itests/linuxshim -Iinclude \
 *     -o tests/linux_mod_harness \
 *     tests/linux_mod_harness.c \
 *     src/modplayer.c src/mod_loader.c src/pokeymax_hw.c src/loop_handler.c \
 *     src/adpcm.c src/tables.c
 *
 * Run:
 *   ./tests/linux_mod_harness bladerun.mod --frames 3000 --trace-boundary
 *
 * Notes:
 * - Uses C loop handler (pokeymax_loop_handler) to emulate sample-end IRQ service.
 * - Timing is approximate; row/pattern/prefetch sequencing is the primary target.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>

#include "modplayer.h"
#include "pokeymax_hw.h"
#include "adpcm.h"
uint8_t pokeymax_irq_pending=0;
//uint32_t POKEYMAX_RAM_SIZE=0;

/* Legacy C handler retained in your tree; useful for desktop sim */
void pokeymax_loop_handler(void);
extern uint8_t pmdbg_last_irq_bits;
extern uint8_t pmdbg_irq_count_total;
extern uint8_t pmdbg_irq_count_ch[4];

/* ---------- Mock PokeyMAX register + sample engine ---------- */

typedef struct {
    uint8_t  configured;
    uint16_t addr;
    uint16_t len_samples;      /* programmed LEN+1 (samples) */
    uint16_t period;
    uint8_t  vol;
    uint8_t  is_adpcm;
    uint8_t  is_8bit;

    uint8_t  dma_on;
    uint8_t  irq_en;

    /* crude simulator state */
    uint32_t samples_remaining_q8; /* in samples<<8 */
    uint32_t play_pos_q16;        /* source sample index in Q16 */

    /* ADPCM renderer state (reset on trigger/retrigger) */
    ADPCMState adpcm_state;
    uint16_t adpcm_decoded_pos;
    int8_t adpcm_last;
} MockChan;

static struct {
    uint8_t regs[0x10000];
    uint8_t * ram;

    uint32_t ram_addr_ptr;
    uint8_t chansel;

    MockChan ch[4];

    uint64_t frame_no;
    uint64_t total_irqs;
    uint64_t total_retriggers;
    uint64_t total_stops;

    int verbose;
    int trace_boundary;
    int use_c_loop_handler;

    /* WAV render */
    int wav_enabled;
    uint32_t wav_rate;
    FILE *wav_fp;
    uint32_t wav_data_bytes;
    uint8_t wav_header_written;
    int wav_gain;            /* linear gain multiplier for mixed sample */
    int interp_linear;       /* 1=linear interpolation, 0=nearest */

    /* Diagnostics */
    int dump_placement;
    int warn_fetch;
    int warn_fetch_limit;         /* max warnings per channel */
    uint32_t warn_fetch_count[4];
    uint32_t warn_fetch_total;
} g;

static void diag_dump_sample_placement(void);
static void diag_warn_fetch(MockChan *c, int ch_idx, uint32_t pos, uint32_t byte_addr, const char *why);
static int  ranges_overlap_u32(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1);

/* ---------- Optional debug tracing / solo ---------- */
typedef struct {
    int enabled;
    int solo_chan;               /* -1 = normal mix, 0..3 = solo */
    int trace_all_frames;        /* 0 = only state changes, 1 = every frame */
    FILE *csv_fp;                /* NULL = disabled */
    const char *csv_path;
} HarnessDebug;

typedef struct {
    uint8_t  valid;
    uint8_t  configured;
    uint8_t  dma_on;
    uint8_t  irq_en;
    uint8_t  is_adpcm;
    uint8_t  is_8bit;
    uint16_t addr;
    uint16_t len_samples;
    uint16_t period;
    uint8_t  vol;
    uint32_t play_pos_q16;
    uint32_t samples_remaining_q8;
} ChanDbgSnap;

static HarnessDebug dbg = {
    .enabled = 0,
    .solo_chan = -1,
    .trace_all_frames = 0,
    .csv_fp = NULL,
    .csv_path = NULL
};
static ChanDbgSnap dbg_prev[4];

/* forward decls for shim macros in tests/linuxshim/pokeymax.h */
uint8_t pokeymax_mock_peek(uint16_t addr);
void    pokeymax_mock_poke(uint16_t addr, uint8_t val);

static void mock_channel_recalc_mode(int idx) {
    uint8_t cfg = g.regs[REG_SAMCFG];
    uint8_t bit = (uint8_t)(1u << idx);
    g.ch[idx].is_adpcm = (cfg & bit) ? 1u : 0u;
    g.ch[idx].is_8bit  = (cfg & (uint8_t)(bit << 4)) ? 1u : 0u;
}

static void mock_channel_start_or_retrigger(int idx) {
    MockChan *c = &g.ch[idx];
    uint32_t len = c->len_samples ? c->len_samples : 1u;
    c->samples_remaining_q8 = len << 8;
    c->play_pos_q16 = 0;
    c->adpcm_state.predictor = 0;
    c->adpcm_state.step_index = 0;
    c->adpcm_decoded_pos = 0;
    c->adpcm_last = 0;
    g.total_retriggers++;
    if (g.verbose) {
        fprintf(stdout,
                "[F=%" PRIu64 "] CH%d trigger addr=%u len=%u per=%u vol=%u mode=%s%s dma=%u irq=%u\n",
                g.frame_no, idx+1, c->addr, c->len_samples, c->period, c->vol,
                c->is_adpcm ? "ADPCM" : "PCM",
                c->is_8bit ? "+8" : "",
                c->dma_on, c->irq_en);
    }
}

static uint32_t period_to_samples_per_frame_q8(uint16_t hw_period, uint8_t is_adpcm) {
    /* Very rough, but monotonic and good enough to generate ends/retriggers.
       PokeyMAX sample rate ~= POKEYMAX_CLOCK / period. We avoid project macros here.
       PAL Paula clock ~= 3.546895 MHz. */
    const uint32_t clock_hz = 3546895u;
    const uint32_t frames_hz = 50u;
    if (hw_period == 0) hw_period = 1;
    uint32_t sps_q8 = (clock_hz << 8) / hw_period;
    uint32_t spf_q8 = sps_q8 / frames_hz;
    if (is_adpcm) {
        /* ADPCM length is in samples in your code, so no extra scaling needed */
    }
    if (spf_q8 == 0) spf_q8 = 1;
    return spf_q8;
}


static void dbg_csv_open(void) {
    if (!dbg.csv_path) return;
    dbg.csv_fp = fopen(dbg.csv_path, "w");
    if (!dbg.csv_fp) {
        fprintf(stderr, "warning: failed to open trace csv: %s\n", dbg.csv_path);
        return;
    }
    fprintf(dbg.csv_fp,
            "frame,chan,event,configured,dma,irq,mode,addr,len,period,vol,play_pos_q16,samples_remaining_q8,irqact,dma_reg,irqen_reg\n");
    fflush(dbg.csv_fp);
}

static void dbg_csv_close(void) {
    if (dbg.csv_fp) {
        fclose(dbg.csv_fp);
        dbg.csv_fp = NULL;
    }
}

static void dbg_snapshot_chan(int i, ChanDbgSnap *s) {
    MockChan *c = &g.ch[i];
    memset(s, 0, sizeof(*s));
    s->valid = 1;
    s->configured = c->configured;
    s->dma_on = c->dma_on;
    s->irq_en = c->irq_en;
    s->is_adpcm = c->is_adpcm;
    s->is_8bit = c->is_8bit;
    s->addr = c->addr;
    s->len_samples = c->len_samples;
    s->period = c->period;
    s->vol = c->vol;
    s->play_pos_q16 = c->play_pos_q16;
    s->samples_remaining_q8 = c->samples_remaining_q8;
}

static const char *dbg_event_name(const ChanDbgSnap *a, const ChanDbgSnap *b) {
    if (!a->valid) return "init";
    if (b->addr != a->addr || b->len_samples != a->len_samples) return "retrigger";
    if (b->period != a->period) return "period";
    if (b->vol != a->vol) return "volume";
    if (b->dma_on != a->dma_on) return b->dma_on ? "dma_on" : "dma_off";
    if (b->irq_en != a->irq_en) return "irq_en";
    if (b->configured != a->configured) return "config";
    if (b->is_adpcm != a->is_adpcm || b->is_8bit != a->is_8bit) return "mode";
    if (b->play_pos_q16 < a->play_pos_q16 && b->dma_on) return "loop_or_restart";
    if (b->samples_remaining_q8 > a->samples_remaining_q8 && b->dma_on) return "reload";
    return "state";
}

static void dbg_trace_frame(void) {
    if (!dbg.csv_fp) return;

    for (int i = 0; i < 4; i++) {
        ChanDbgSnap now;
        dbg_snapshot_chan(i, &now);

        int changed = 0;
        if (!dbg_prev[i].valid) {
            changed = 1;
        } else if (memcmp(&dbg_prev[i], &now, sizeof(now)) != 0) {
            changed = 1;
        }

        if (dbg.trace_all_frames || changed) {
            const char *ev = dbg_event_name(&dbg_prev[i], &now);
            fprintf(dbg.csv_fp,
                    "%" PRIu64 ",%d,%s,%u,%u,%u,%s%s,%u,%u,%u,%u,%" PRIu32 ",%" PRIu32 ",%02X,%02X,%02X\n",
                    g.frame_no, i, ev,
                    (unsigned)now.configured,
                    (unsigned)now.dma_on,
                    (unsigned)now.irq_en,
                    now.is_adpcm ? "ADPCM" : "PCM",
                    now.is_8bit ? "+8" : "",
                    (unsigned)now.addr,
                    (unsigned)now.len_samples,
                    (unsigned)now.period,
                    (unsigned)now.vol,
                    now.play_pos_q16,
                    now.samples_remaining_q8,
                    (unsigned)(pokeymax_mock_peek(REG_IRQACT) & 0x0F),
                    (unsigned)(pokeymax_mock_peek(REG_DMA) & 0x0F),
                    (unsigned)(pokeymax_mock_peek(REG_IRQEN) & 0x0F));
            dbg_prev[i] = now;
        }
    }
    fflush(dbg.csv_fp);
}


static void mock_step_sample_engine_one_vbi(void) {
    /* Advance each active DMA channel; set IRQACT bits on end if IRQ enabled. */
    for (int i = 0; i < 4; i++) {
        MockChan *c = &g.ch[i];
        uint8_t bit = (uint8_t)(1u << i);
        c->dma_on = (g.regs[REG_DMA] & bit) ? 1u : 0u;
        c->irq_en = (g.regs[REG_IRQEN] & bit) ? 1u : 0u;
        if (!c->dma_on || !c->configured) continue;

        uint32_t dec_q8 = period_to_samples_per_frame_q8(c->period ? c->period : 1u, c->is_adpcm);
        if (c->samples_remaining_q8 > dec_q8) {
            c->samples_remaining_q8 -= dec_q8;
            continue;
        }

        c->samples_remaining_q8 = 0;
        if (c->irq_en) {
            g.regs[REG_IRQACT] |= bit;
            g.total_irqs++;
            if (g.verbose) {
                fprintf(stdout, "[F=%" PRIu64 "] CH%d end -> IRQACT=%02X\n",
                        g.frame_no, i+1, g.regs[REG_IRQACT] & 0x0F);
            }
        }
    }
}

static void mock_service_loop_irqs_if_needed(void) {
    if (!g.use_c_loop_handler) return;
    if ((pokeymax_mock_peek(REG_IRQACT) & 0x0Fu) == 0) return;
    pmdbg_last_irq_bits = pokeymax_mock_peek(REG_IRQACT) & 0x0Fu;
    pmdbg_irq_count_total++;
    for (int ch = 0; ch < 4; ch++) if (pmdbg_last_irq_bits & (1u<<ch)) pmdbg_irq_count_ch[ch]++;
    pokeymax_loop_handler();
}

/* Shim functions called by PEEK/POKE macros */
uint8_t pokeymax_mock_peek(uint16_t addr) {
    return g.regs[addr];
}

void pokeymax_mock_poke(uint16_t addr, uint8_t val) {
    switch (addr) {
        case REG_RAMADDRL:
            g.ram_addr_ptr = (uint16_t)((g.ram_addr_ptr & 0xFF00u) | val);
            g.regs[addr] = val;
            return;
        case REG_RAMADDRH:
            g.ram_addr_ptr = (uint16_t)((g.ram_addr_ptr & 0x00FFu) | ((uint16_t)val << 8));
            g.regs[addr] = val;
            return;
        case REG_RAMDATAINC:
            if (g.ram_addr_ptr < POKEYMAX_RAM_SIZE) {
                g.ram[g.ram_addr_ptr] = val;
            }
            g.ram_addr_ptr++;
            g.regs[addr] = val;
            return;
        case REG_CHANSEL:
            g.chansel = val;
            g.regs[addr] = val;
            return;
        case REG_ADDRL:
        case REG_ADDRH:
        case REG_LENL:
        case REG_LENH:
        case REG_PERL:
        case REG_PERH:
        case REG_VOL:
            g.regs[addr] = val;
            if (g.chansel >= 1 && g.chansel <= 4) {
                MockChan *c = &g.ch[g.chansel - 1];
                c->configured = 1;
                switch (addr) {
                    case REG_ADDRL:
                        c->addr = (uint16_t)((c->addr & 0xFF00u) | val);
                        break;
                    case REG_ADDRH:
                        c->addr = (uint16_t)((c->addr & 0x00FFu) | ((uint16_t)val << 8));
                        break;
                    case REG_LENL: {
                        uint16_t lminus1 = (uint16_t)((c->len_samples ? (c->len_samples - 1u) : 0u) & 0xFF00u);
                        lminus1 = (uint16_t)(lminus1 | val);
                        c->len_samples = (uint16_t)(lminus1 + 1u);
                        break;
                    }
                    case REG_LENH: {
                        uint16_t lminus1 = (uint16_t)((c->len_samples ? (c->len_samples - 1u) : 0u) & 0x00FFu);
                        lminus1 = (uint16_t)(lminus1 | ((uint16_t)val << 8));
                        c->len_samples = (uint16_t)(lminus1 + 1u);
                        break;
                    }
                    case REG_PERL:
                        c->period = (uint16_t)((c->period & 0x0F00u) | val);
                        break;
                    case REG_PERH:
                        c->period = (uint16_t)((c->period & 0x00FFu) | (((uint16_t)val & 0x0Fu) << 8));
                        break;
                    case REG_VOL:
                        c->vol = (uint8_t)(val & 0x3Fu);
                        break;
                }
                mock_channel_recalc_mode(g.chansel - 1);
            }
            return;
        case REG_SAMCFG:
            g.regs[addr] = val;
            for (int i = 0; i < 4; i++) mock_channel_recalc_mode(i);
            return;
        case REG_IRQACT:
            /* PokeyMAX semantics: write 0 clears bit; write 1 keeps bit */
            g.regs[REG_IRQACT] &= val;
            return;
        case REG_DMA: {
            uint8_t old = g.regs[REG_DMA] & 0x0F;
            uint8_t now = val & 0x0F;
            g.regs[addr] = (g.regs[addr] & 0xF0) | now;
            for (int i = 0; i < 4; i++) {
                uint8_t bit = (uint8_t)(1u << i);
                uint8_t oldb = old & bit;
                uint8_t newb = now & bit;
                g.ch[i].dma_on = newb ? 1u : 0u;
                if (oldb != newb && newb) {
                    mock_channel_start_or_retrigger(i);
                }
            }
            return;
        }
        case REG_IRQEN:
            g.regs[addr] = (g.regs[addr] & 0xF0) | (val & 0x0F);
            for (int i = 0; i < 4; i++) g.ch[i].irq_en = (val >> i) & 1u;
            return;
        default:
            g.regs[addr] = val;
            return;
    }
}

/* --------- Stubs / exports expected when not linking Atari ISR asm --------- */
uint8_t pmdbg_last_irq_bits = 0;
uint8_t pmdbg_irq_count_total = 0;
uint8_t pmdbg_irq_count_ch[4] = {0,0,0,0};
void vbi_install(void) {}
void vbi_remove(void) {}


/* ---------- WAV output + crude mixer ---------- */

static void wav_write_u16le(FILE *fp, uint16_t v) {
    fputc((int)(v & 0xFFu), fp);
    fputc((int)((v >> 8) & 0xFFu), fp);
}
static void wav_write_u32le(FILE *fp, uint32_t v) {
    fputc((int)(v & 0xFFu), fp);
    fputc((int)((v >> 8) & 0xFFu), fp);
    fputc((int)((v >> 16) & 0xFFu), fp);
    fputc((int)((v >> 24) & 0xFFu), fp);
}
static int wav_begin(const char *path, uint32_t rate_hz) {
    g.wav_fp = fopen(path, "wb");
    if (!g.wav_fp) return -1;
    g.wav_rate = rate_hz ? rate_hz : 44100u;
    g.wav_data_bytes = 0;
    g.wav_enabled = 1;
    /* placeholder header */
    fwrite("RIFF", 1, 4, g.wav_fp);
    wav_write_u32le(g.wav_fp, 0);
    fwrite("WAVE", 1, 4, g.wav_fp);
    fwrite("fmt ", 1, 4, g.wav_fp);
    wav_write_u32le(g.wav_fp, 16);         /* PCM fmt chunk size */
    wav_write_u16le(g.wav_fp, 1);          /* PCM */
    wav_write_u16le(g.wav_fp, 1);          /* mono */
    wav_write_u32le(g.wav_fp, g.wav_rate);
    wav_write_u32le(g.wav_fp, g.wav_rate * 2u); /* byte rate */
    wav_write_u16le(g.wav_fp, 2);          /* block align */
    wav_write_u16le(g.wav_fp, 16);         /* bits/sample */
    fwrite("data", 1, 4, g.wav_fp);
    wav_write_u32le(g.wav_fp, 0);
    g.wav_header_written = 1;
    return 0;
}
static void wav_write_sample_i16(int16_t s) {
    if (!g.wav_enabled || !g.wav_fp) return;
    wav_write_u16le(g.wav_fp, (uint16_t)s);
    g.wav_data_bytes += 2u;
}
static void wav_finish(void) {
    if (!g.wav_enabled || !g.wav_fp || !g.wav_header_written) return;
    long endpos = ftell(g.wav_fp);
    if (endpos >= 0) {
        fseek(g.wav_fp, 4, SEEK_SET);
        wav_write_u32le(g.wav_fp, 36u + g.wav_data_bytes);
        fseek(g.wav_fp, 40, SEEK_SET);
        wav_write_u32le(g.wav_fp, g.wav_data_bytes);
        fseek(g.wav_fp, endpos, SEEK_SET);
    }
    fclose(g.wav_fp);
    g.wav_fp = NULL;
}

static int ranges_overlap_u32(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
    /* Half-open intervals [a0,a1), [b0,b1) */
    return (a0 < b1) && (b0 < a1);
}

static void diag_dump_sample_placement(void) {
    /* Uses final packed sample info from mod loader/player state. */
    uint32_t max_end = 0;
    int any = 0;

    printf("=== Sample placement dump ===\n");
    printf("RAM size compile-time POKEYMAX_RAM_SIZE=%u bytes\n", (unsigned)POKEYMAX_RAM_SIZE);
    printf("Idx  Addr   End    LenSt  SrcLen LoopStart LoopLen Mode   DS Name\n");
    printf("---- ------ ------ ------ ------ --------- ------- ------ -- ----------------------\n");

    for (int i = 1; i <= MOD_MAX_SAMPLES; i++) {
        const SampleInfo *s = &mod.samples[i];
        if (s->length == 0 && s->pokeymax_len == 0) continue;
        any = 1;

        uint32_t addr = (uint32_t)s->pokeymax_addr;
        uint32_t len_st = (uint32_t)s->pokeymax_len;
        uint32_t end = addr + len_st; /* half-open */
        if (end > max_end) max_end = end;

        uint8_t ds_shift = SI_DS_SHIFT(s);
        uint8_t ds_factor = (uint8_t)(1u << ds_shift);  /* 1, 2, 4, or 8 */
        printf("%3d  %5u  %5u  %5u  %5u  %8u %7u %-6s %2u\n",
               i,
               (unsigned)addr,
               (unsigned)(end ? (end - 1u) : addr),
               (unsigned)len_st,
               (unsigned)s->length,
               (unsigned)s->loop_start,
               (unsigned)s->loop_len,
               SI_IS_ADPCM(s) ? "ADPCM" : "PCM",
               (unsigned)(ds_factor ? ds_factor : 1u)
               );

        if (len_st == 0) {
            printf("  !! S%02d has zero stored length\n", i);
        }
        if (end > (uint32_t)POKEYMAX_RAM_SIZE) {
            printf("  !! S%02d exceeds POKEYMAX_RAM_SIZE: addr=%u len=%u end=%u > %u\n",
                   i, (unsigned)addr, (unsigned)len_st, (unsigned)end, (unsigned)POKEYMAX_RAM_SIZE);
        }
        if (end > 65536u) {
            printf("  !! S%02d crosses 16-bit address space (0x10000): addr=%u len=%u end=%u\n",
                   i, (unsigned)addr, (unsigned)len_st, (unsigned)end);
        }
        if (SI_HAS_LOOP(s)) {
            uint32_t loop_end_src = (uint32_t)s->loop_start + (uint32_t)s->loop_len;
            if (loop_end_src > (uint32_t)s->length) {
                printf("  !! S%02d source loop exceeds source length: loop_end=%u > src_len=%u\n",
                       i, (unsigned)loop_end_src, (unsigned)s->length);
            }
        }
    }

    if (!any) {
        printf("(no samples)\n");
    } else {
        printf("Max stored RAM end (exclusive): %u\n", (unsigned)max_end);
    }

    /* Overlap scan on stored regions */
    for (int i = 1; i <= MOD_MAX_SAMPLES; i++) {
        const SampleInfo *a = &mod.samples[i];
        uint32_t a0 = (uint32_t)a->pokeymax_addr;
        uint32_t a1 = a0 + (uint32_t)a->pokeymax_len;
        if (a->pokeymax_len == 0) continue;
        for (int j = i + 1; j <= MOD_MAX_SAMPLES; j++) {
            const SampleInfo *b = &mod.samples[j];
            uint32_t b0 = (uint32_t)b->pokeymax_addr;
            uint32_t b1 = b0 + (uint32_t)b->pokeymax_len;
            if (b->pokeymax_len == 0) continue;
            if (ranges_overlap_u32(a0, a1, b0, b1)) {
                printf("  !! OVERLAP S%02d [%u..%u) with S%02d [%u..%u)\n",
                       i, (unsigned)a0, (unsigned)a1, j, (unsigned)b0, (unsigned)b1);
            }
        }
    }
    printf("=== End placement dump ===\n");
}

static void diag_warn_fetch(MockChan *c, int ch_idx, uint32_t pos, uint32_t byte_addr, const char *why) {
    if (!g.warn_fetch) return;
    if (ch_idx < 0 || ch_idx >= 4) return;
    if (g.warn_fetch_limit > 0 && g.warn_fetch_count[ch_idx] >= (uint32_t)g.warn_fetch_limit) return;

    g.warn_fetch_count[ch_idx]++;
    g.warn_fetch_total++;

    fprintf(stderr,
            "[WARN_FETCH F=%" PRIu64 " CH%d] %s addr=%u pos=%u byte_addr=%u len=%u per=%u vol=%u mode=%s%s dma=%u irq=%u play_pos_q16=%" PRIu32 "\n",
            g.frame_no, ch_idx + 1, why,
            (unsigned)c->addr,
            (unsigned)pos,
            (unsigned)byte_addr,
            (unsigned)c->len_samples,
            (unsigned)c->period,
            (unsigned)c->vol,
            c->is_adpcm ? "ADPCM" : "PCM",
            c->is_8bit ? "+8" : "",
            (unsigned)c->dma_on,
            (unsigned)c->irq_en,
            c->play_pos_q16);
}


static int8_t mock_fetch_pcm8(MockChan *c) {
    uint16_t pos = (uint16_t)(c->play_pos_q16 >> 16);
    if (pos >= c->len_samples) return 0;

    if (!c->is_adpcm) {
        uint32_t a = (uint32_t)c->addr + pos;
        if (a >= 65536u) {
            diag_warn_fetch(c, (int)(c - &g.ch[0]), (uint32_t)pos, a, "PCM fetch crosses 16-bit address");
        }
        if (a >= POKEYMAX_RAM_SIZE) {
            diag_warn_fetch(c, (int)(c - &g.ch[0]), (uint32_t)pos, a, "PCM fetch out of sample RAM");
            return 0;
        }
        return (int8_t)g.ram[a];
    }

    while (c->adpcm_decoded_pos <= pos) {
        uint16_t sample_idx = c->adpcm_decoded_pos;
        uint32_t byte_addr = (uint32_t)c->addr + (uint32_t)(sample_idx >> 1);
        uint8_t byte;
        uint8_t nibble;
        if (byte_addr >= 65536u) {
            diag_warn_fetch(c, (int)(c - &g.ch[0]), (uint32_t)sample_idx, byte_addr, "ADPCM fetch crosses 16-bit address");
        }
        if (byte_addr >= POKEYMAX_RAM_SIZE) {
            diag_warn_fetch(c, (int)(c - &g.ch[0]), (uint32_t)sample_idx, byte_addr, "ADPCM fetch out of sample RAM");
            return 0;
        }

        byte = g.ram[byte_addr];
        nibble = (uint8_t)((sample_idx & 1u) ? (byte & 0x0Fu) : ((byte >> 4) & 0x0Fu));
        c->adpcm_last = adpcm_decode_nibble(nibble, &c->adpcm_state);
        c->adpcm_decoded_pos++;
    }

    return c->adpcm_last;
}
static int16_t mock_fetch_pcm8_linear_safe(const MockChan *c);

static uint32_t period_to_samples_per_out_q16(uint16_t hw_period, uint32_t out_rate_hz) {
    const uint32_t clock_hz = 3546895u;
    if (hw_period == 0) hw_period = 1;
    if (out_rate_hz == 0) out_rate_hz = 44100u;
    /* source sample rate ~= clock / period; convert to source-samples/output-sample in Q16 */
    uint32_t inc_q16 = (uint32_t)(((uint64_t)clock_hz << 16) / ((uint64_t)hw_period * out_rate_hz));
    if (inc_q16 == 0) inc_q16 = 1;
    return inc_q16;
}
static void mock_render_audio_for_one_vbi(void) {
    if (!g.wav_enabled || !g.wav_fp) return;
    uint32_t nsamp = (g.wav_rate + 25u) / 50u; /* ~882 @44.1kHz */
    for (uint32_t n = 0; n < nsamp; n++) {
        int mix = 0;
        for (int i = 0; i < 4; i++) {
            MockChan *c = &g.ch[i];
            uint8_t bit = (uint8_t)(1u << i);
            c->dma_on = (g.regs[REG_DMA] & bit) ? 1u : 0u;
            c->irq_en = (g.regs[REG_IRQEN] & bit) ? 1u : 0u;
            if (!c->dma_on || !c->configured) continue;

            if (dbg.solo_chan >= 0 && i != dbg.solo_chan) {
                /* still advance timing/state, but mute contribution */
                /* (do not 'continue' here, to preserve loop/retrigger behavior) */
            }

            int16_t s8 = g.interp_linear ? mock_fetch_pcm8_linear_safe(c) : (int16_t)mock_fetch_pcm8(c);

            /* simple linear-ish volume scaling (0..64-ish) */
            int v = (int)(c->vol & 0x3Fu);
            if (!(dbg.solo_chan >= 0 && i != dbg.solo_chan)) {
                mix += ((int)s8 * v) / 32;
            }

            /* advance source position (rough timing), but service end IRQs immediately */
            uint32_t inc_q16 = period_to_samples_per_out_q16(c->period ? c->period : 1u, g.wav_rate);
            uint32_t guard = 0;
            while (inc_q16) {
                uint16_t len = c->len_samples;
                if (!len) {
                    c->play_pos_q16 = 0;
                    break;
                }

                uint32_t len_q16 = ((uint32_t)len << 16);
                uint32_t pos_q16 = c->play_pos_q16;
                if (pos_q16 >= len_q16) pos_q16 = c->play_pos_q16 = ((uint32_t)(len - 1u) << 16);

                /* amount to reach end boundary (exclusive) */
                uint32_t to_end_q16 = (len_q16 > pos_q16) ? (len_q16 - pos_q16) : 0u;
                if (to_end_q16 == 0u) to_end_q16 = 1u;

                if (inc_q16 < to_end_q16) {
                    c->play_pos_q16 = pos_q16 + inc_q16;
                    inc_q16 = 0;
                    break;
                }

                /* consume up to/past end */
                inc_q16 -= to_end_q16;
                c->play_pos_q16 = ((uint32_t)(len - 1u) << 16);

                if (c->irq_en) {
                    g.regs[REG_IRQACT] |= bit;
                    g.total_irqs++;
                }
                mock_service_loop_irqs_if_needed();

                /* refresh state after possible retrigger/reprogram by loop handler */
                c = &g.ch[i];
                c->dma_on = (g.regs[REG_DMA] & bit) ? 1u : 0u;
                c->irq_en = (g.regs[REG_IRQEN] & bit) ? 1u : 0u;

                if (!c->dma_on) break;

                /* If no retrigger occurred, avoid infinite looping at sample end */
                if (c->play_pos_q16 >= (((uint32_t)(c->len_samples ? c->len_samples : 1u) << 16) - 1u)) {
                    /* one-shot / no immediate loop service path: stay clamped */
                    break;
                }

                if (++guard > 64u) {
                    /* Safety: absurdly tiny loops at high pitch can wrap many times per output sample */
                    break;
                }
            }
        }
        mix *= g.wav_gain;
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        wav_write_sample_i16((int16_t)mix);
    }
}

/* ---------- Harness helpers ---------- */
static void dump_pos(const char *tag) {
    printf("[%s] order=%u row=%u tick=%u playing=%u need_prefetch=%u IRQACT=%02X DMA=%02X IRQEN=%02X\n",
           tag,
           (unsigned)mod_get_order(), (unsigned)mod_get_row(), (unsigned)mod.tick,
           (unsigned)mod.playing, (unsigned)mod_need_prefetch,
           (unsigned)pokeymax_mock_peek(REG_IRQACT),
           (unsigned)pokeymax_mock_peek(REG_DMA),
           (unsigned)pokeymax_mock_peek(REG_IRQEN));
}

/* Interpolated PCM fetch for WAV rendering only (does not affect player logic).
 * Uses c->play_pos_q16 as source position in samples (16.16 fixed point).
 * Assumes PCM8 path (same source format as mock_fetch_pcm8).
 */
static int8_t mock_fetch_pcm8_at_index(const MockChan *c, uint32_t idx) {
    /* PCM8 only helper (no ADPCM decode path) */
    uint32_t a = (uint32_t)c->addr + idx;
    if (a >= POKEYMAX_RAM_SIZE) return 0;
    return (int8_t)g.ram[a];
}

static int16_t mock_fetch_pcm8_linear_safe(const MockChan *c) {
    if (!c->configured || !c->dma_on || c->len_samples == 0) return 0;
    if (c->is_adpcm) {
        /* Do not interpolate compressed source bytes */
        return (int16_t)mock_fetch_pcm8((MockChan *)c);
    }

    uint32_t pos_q16 = c->play_pos_q16;
    uint32_t idx = (pos_q16 >> 16);
    uint32_t frac = pos_q16 & 0xFFFFu; /* 0..65535 */

    uint32_t len = (uint32_t)c->len_samples;
    if (len == 0) return 0;
    if (idx >= len) {
        /* Out of range due to stepping edge case: nearest fallback */
        return (int16_t)mock_fetch_pcm8((MockChan *)c);
    }

    /* Boundary case: idx is last sample in current programmed block.
     * The true "next" sample may be from a loop retrigger that hasn't been serviced yet,
     * so interpolating here creates whistles. Use nearest instead.
     */
    if (idx + 1u >= len) {
        return (int16_t)mock_fetch_pcm8((MockChan *)c);
    }

    /* If exactly on a sample point, skip interpolation math */
    if (frac == 0u) {
        return (int16_t)mock_fetch_pcm8_at_index(c, idx);
    }

    int32_t s0 = (int32_t)mock_fetch_pcm8_at_index(c, idx);
    int32_t s1 = (int32_t)mock_fetch_pcm8_at_index(c, idx + 1u);

    /* Linear interpolation in 16.16 fraction domain */
    int32_t y = ((s0 * (int32_t)(65536u - frac)) + (s1 * (int32_t)frac)) >> 16;
    if (y < -128) y = -128;
    if (y >  127) y = 127;
    return (int16_t)y;
}


static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <modfile> [--frames N] [--verbose] [--trace-boundary] [--no-loop-handler] [--wav out.wav] [--rate Hz] [--gain N] [--dump-placement] [--warn-fetch] [--warn-fetch-limit N]\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *modfile = NULL;
    uint32_t max_frames = 5000;
    int use_c_loop_handler = 1;
    const char *wav_path = NULL;
    double seconds_override = 0.0;

    memset(&g, 0, sizeof(g));
    memset(dbg_prev, 0, sizeof(dbg_prev));
    g.interp_linear = 1;      /* default to linear interpolation */
    g.regs[REG_SAMCFG] = 0xF0; /* reset default */
    g.wav_gain = 96;          /* decent starting point for 4x 8-bit mix */
    g.dump_placement = 0;
    g.warn_fetch = 0;
    g.warn_fetch_limit = 8;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds_override = strtod(argv[++i], NULL);
            if (seconds_override < 0.0) seconds_override = 0.0;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g.verbose = 1;
        } else if (strcmp(argv[i], "--trace-boundary") == 0) {
            g.trace_boundary = 1;
        } else if (strcmp(argv[i], "--no-loop-handler") == 0) {
            use_c_loop_handler = 0;
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            g.wav_rate = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
            g.wav_gain = (int)strtol(argv[++i], NULL, 0);
            if (g.wav_gain < 1) g.wav_gain = 1;
        } else if (strcmp(argv[i], "--dump-placement") == 0) {
            g.dump_placement = 1;
        } else if (strcmp(argv[i], "--warn-fetch") == 0) {
            g.warn_fetch = 1;
        } else if (strcmp(argv[i], "--warn-fetch-limit") == 0 && i + 1 < argc) {
            g.warn_fetch_limit = (int)strtol(argv[++i], NULL, 0);
            if (g.warn_fetch_limit < 0) g.warn_fetch_limit = 0;
        } else if (strcmp(argv[i], "--warn-fetch-limit") == 0) {
            fprintf(stderr, "--warn-fetch-limit requires a value\n"); return 2;
        } else if (strcmp(argv[i], "--solo") == 0 && i + 1 < argc) {
            dbg.solo_chan = (int)strtol(argv[++i], NULL, 0);
            if (dbg.solo_chan < 0 || dbg.solo_chan > 3) {
                fprintf(stderr, "invalid --solo channel (expected 0..3)\n");
                return 2;
            }
            dbg.enabled = 1;
        } else if (strcmp(argv[i], "--interp-nearest") == 0) {
            g.interp_linear = 0;
            dbg.enabled = 1;
        } else if (strcmp(argv[i], "--trace-csv") == 0 && i + 1 < argc) {
            dbg.csv_path = argv[++i];
            dbg.enabled = 1;
        } else if (strcmp(argv[i], "--trace-all-frames") == 0) {
            dbg.trace_all_frames = 1;
            dbg.enabled = 1;
//        } else if (strcmp(argv[i], "--ram-size") == 0 && i + 1 < argc) {
//            POKEYMAX_RAM_SIZE = atol(argv[++i]);
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            modfile = argv[i];
        }
    }
    g.ram = malloc(POKEYMAX_RAM_SIZE);
    if (!modfile) {
        usage(argv[0]);
        return 2;
    }
    if (seconds_override > 0.0) {
        /* 50 VBI/sec */
        max_frames = (uint32_t)(seconds_override * 50.0 + 0.5);
    }

    if (mod_load(modfile) != 0u) {
        fprintf(stderr, "mod_load failed for %s\n", modfile);
        return 1;
    }
    mod_play();

    if (g.dump_placement) {
        diag_dump_sample_placement();
    }

    if (wav_path) {
        if (wav_begin(wav_path, g.wav_rate ? g.wav_rate : 44100u) != 0) {
            fprintf(stderr, "failed to open wav output %s\n", wav_path);
            mod_stop();
            mod_file_close();
            return 1;
        }
        printf("WAV output: %s @ %u Hz (gain=%d)\n", wav_path, (unsigned)g.wav_rate, g.wav_gain);
    }

    if (dbg.csv_path) {
        dbg_csv_open();
        if (dbg.csv_fp) {
            printf("CSV trace: %s%s\n", dbg.csv_path,
                   dbg.trace_all_frames ? " (all frames)" : " (state changes)");
        }
    }
    if (dbg.solo_chan >= 0) printf("Solo channel: %d\n", dbg.solo_chan);
    if (g.wav_enabled) printf("Interpolation: %s\n", g.interp_linear ? "linear" : "nearest");

    g.use_c_loop_handler = use_c_loop_handler;

    dump_pos("start");

    uint8_t prev_order = mod_get_order();
    uint8_t prev_row = mod_get_row();
    uint8_t prev_tick = mod.tick;

    for (g.frame_no = 0; g.frame_no < max_frames && mod.playing; g.frame_no++) {
        if (g.wav_enabled) {
            /* Accurate desktop debug path: render advances sample engine and services loop IRQs immediately. */
            mock_render_audio_for_one_vbi();
        } else {
            /* Fast non-audio path: VBI-granularity hardware progress is good enough for tracing. */
            mock_step_sample_engine_one_vbi();
            mock_service_loop_irqs_if_needed();
        }

        if (dbg.csv_fp) {
            dbg_trace_frame();
        }

        /* VBI tick (player logic) */
        pokeymax_mock_poke(RTCLOK,pokeymax_mock_peek(RTCLOK)+1);
        mod_vbi_tick();

        /* Main loop prefetch polling */
        if (mod_need_prefetch) {
            if (g.trace_boundary) {
                printf("[F=%" PRIu64 "] prefetch requested @ order=%u row=%u tick=%u\n",
                       g.frame_no, (unsigned)mod_get_order(), (unsigned)mod_get_row(), (unsigned)mod.tick);
            }
            if (mod_prefetch_next_pattern() != 0u) {
                fprintf(stderr, "prefetch failed at frame=%" PRIu64 " order=%u row=%u\n",
                        g.frame_no, (unsigned)mod_get_order(), (unsigned)mod_get_row());
                break;
            }
            if (g.trace_boundary) {
                printf("[F=%" PRIu64 "] prefetch complete\n", g.frame_no);
            }
        }

        if (mod_get_order() != prev_order || mod_get_row() != prev_row || mod.tick != prev_tick) {
            if (g.trace_boundary) {
                /* Emit detailed trace around row 60..2 boundary or on order change */
                uint8_t r = mod_get_row();
                if ((prev_row >= 60u) || (r <= 3u) || (mod_get_order() != prev_order)) {
                    printf("[F=%" PRIu64 "] pos %u:%u:%u -> %u:%u:%u need_prefetch=%u IRQACT=%02X DMA=%02X IRQEN=%02X\n",
                           g.frame_no,
                           (unsigned)prev_order, (unsigned)prev_row, (unsigned)prev_tick,
                           (unsigned)mod_get_order(), (unsigned)mod_get_row(), (unsigned)mod.tick,
                           (unsigned)mod_need_prefetch,
                           (unsigned)pokeymax_mock_peek(REG_IRQACT),
                           (unsigned)pokeymax_mock_peek(REG_DMA),
                           (unsigned)pokeymax_mock_peek(REG_IRQEN));
                }
            }
            prev_order = mod_get_order();
            prev_row = mod_get_row();
            prev_tick = mod.tick;
        }
    }

    dump_pos("end");
    printf("frames=%" PRIu64 " total_irqs=%" PRIu64 " retriggers=%" PRIu64 "\n",
           g.frame_no, g.total_irqs, g.total_retriggers);
    printf("pmdbg irq_count_total=%u ch=[%u %u %u %u]\n",
           (unsigned)pmdbg_irq_count_total,
           (unsigned)pmdbg_irq_count_ch[0], (unsigned)pmdbg_irq_count_ch[1],
           (unsigned)pmdbg_irq_count_ch[2], (unsigned)pmdbg_irq_count_ch[3]);

    if (g.warn_fetch) {
        printf("warn_fetch_total=%u counts=[%u %u %u %u]\n", (unsigned)g.warn_fetch_total, (unsigned)g.warn_fetch_count[0], (unsigned)g.warn_fetch_count[1], (unsigned)g.warn_fetch_count[2], (unsigned)g.warn_fetch_count[3]);
    }

    if (g.wav_enabled) {
        wav_finish();
        printf("wrote wav bytes=%u (%.2f sec)\n", (unsigned)g.wav_data_bytes,
               g.wav_rate ? (double)g.wav_data_bytes / (2.0 * (double)g.wav_rate) : 0.0);
    }
    dbg_csv_close();

    mod_stop();
    mod_file_close();
    return 0;
}
