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

#include "modplayer.h"
#include "pokeymax_hw.h"
uint8_t pokeymax_irq_pending=0;

/* Legacy C handler retained in your tree; useful for desktop sim */
void pokeymax_loop_handler(void);

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
} MockChan;

static struct {
    uint8_t regs[0x10000];
    uint8_t ram[POKEYMAX_RAM_SIZE];

    uint16_t ram_addr_ptr;
    uint8_t chansel;

    MockChan ch[4];

    uint64_t frame_no;
    uint64_t total_irqs;
    uint64_t total_retriggers;
    uint64_t total_stops;

    int verbose;
    int trace_boundary;

    /* WAV render */
    int wav_enabled;
    uint32_t wav_rate;
    FILE *wav_fp;
    uint32_t wav_data_bytes;
    uint8_t wav_header_written;
    int wav_gain;            /* linear gain multiplier for mixed sample */
} g;

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
                c->addr = (uint16_t)(g.regs[REG_ADDRL] | ((uint16_t)g.regs[REG_ADDRH] << 8));
                c->len_samples = (uint16_t)(1u + g.regs[REG_LENL] + ((uint16_t)g.regs[REG_LENH] << 8));
                c->period = (uint16_t)(g.regs[REG_PERL] | ((uint16_t)(g.regs[REG_PERH] & 0x0Fu) << 8));
                c->vol = (uint8_t)(g.regs[REG_VOL] & 0x3Fu);
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
static int8_t mock_fetch_pcm8(const MockChan *c) {
    uint16_t pos = (uint16_t)(c->play_pos_q16 >> 16);
    if (pos >= c->len_samples) return 0;
    uint32_t a = (uint32_t)c->addr + pos;
    if (a >= POKEYMAX_RAM_SIZE) return 0;
    return (int8_t)g.ram[a];
}
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
            if (!c->dma_on || !c->configured) continue;

            int8_t s = mock_fetch_pcm8(c);

            /* simple linear-ish volume scaling (0..64-ish) */
            int v = (int)(c->vol & 0x3Fu);
            mix += ((int)s * v) / 32;

            /* advance source position (rough timing) */
            c->play_pos_q16 += period_to_samples_per_out_q16(c->period ? c->period : 1u, g.wav_rate);
            {
                uint16_t pos = (uint16_t)(c->play_pos_q16 >> 16);
                if (pos >= c->len_samples) {
                    /* clamp at end until loop handler retriggers on next VBI */
                    if (c->len_samples) c->play_pos_q16 = ((uint32_t)(c->len_samples - 1u) << 16);
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

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <modfile> [--frames N] [--verbose] [--trace-boundary] [--no-loop-handler] [--wav out.wav] [--rate Hz] [--gain N]\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *modfile = NULL;
    uint32_t max_frames = 5000;
    int use_c_loop_handler = 1;
    const char *wav_path = NULL;

    memset(&g, 0, sizeof(g));
    g.regs[REG_SAMCFG] = 0xF0; /* reset default */
    g.wav_gain = 96;          /* decent starting point for 4x 8-bit mix */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
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
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            modfile = argv[i];
        }
    }
    if (!modfile) {
        usage(argv[0]);
        return 2;
    }

    if (mod_load(modfile) != 0u) {
        fprintf(stderr, "mod_load failed for %s\n", modfile);
        return 1;
    }
    mod_play();

    if (wav_path) {
        if (wav_begin(wav_path, g.wav_rate ? g.wav_rate : 44100u) != 0) {
            fprintf(stderr, "failed to open wav output %s\n", wav_path);
            mod_stop();
            mod_file_close();
            return 1;
        }
        printf("WAV output: %s @ %u Hz (gain=%d)\n", wav_path, (unsigned)g.wav_rate, g.wav_gain);
    }

    dump_pos("start");

    uint8_t prev_order = mod_get_order();
    uint8_t prev_row = mod_get_row();
    uint8_t prev_tick = mod.tick;

    for (g.frame_no = 0; g.frame_no < max_frames && mod.playing; g.frame_no++) {
        /* Simulate hardware progress and IRQ generation */
        mock_step_sample_engine_one_vbi();

        /* Service sample-end IRQs via legacy C helper (desktop approximation) */
        if (use_c_loop_handler && (pokeymax_mock_peek(REG_IRQACT) & 0x0Fu)) {
            pmdbg_last_irq_bits = pokeymax_mock_peek(REG_IRQACT) & 0x0Fu;
            pmdbg_irq_count_total++;
            for (int ch = 0; ch < 4; ch++) if (pmdbg_last_irq_bits & (1u<<ch)) pmdbg_irq_count_ch[ch]++;
            pokeymax_loop_handler();
        }

        /* Render audible output for this frame window (crude desktop mix) */
        mock_render_audio_for_one_vbi();

        /* VBI tick (player logic) */
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

    if (g.wav_enabled) {
        wav_finish();
        printf("wrote wav bytes=%u (%.2f sec)\n", (unsigned)g.wav_data_bytes,
               g.wav_rate ? (double)g.wav_data_bytes / (2.0 * (double)g.wav_rate) : 0.0);
    }

    mod_stop();
    mod_file_close();
    return 0;
}
