/*
 * test13_effect_format_parity_linux.c
 *
 * Linux unit test that checks effect processing is format-agnostic and that
 * downsample pitch compensation is applied consistently across *all* hardware
 * period writes.
 *
 * It does NOT render audio. Instead it records pokeymax_channel_setup() and
 * pokeymax_channel_set_period_vol() calls made by modplayer.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define UNIT_TEST 1

#include "modplayer.h"
#include "mod_format.h"
#include "pokeymax_hw.h" /* for prototypes */

/* ---- Unit-test hooks from modplayer.c (enabled by -DUNIT_TEST) ---- */
void mod_test_reset_state(void);
void mod_test_apply_row_bytes(const uint8_t *row16);
void mod_test_set_tick(uint8_t t);
void mod_test_update_effects(void);

/* ---- Mock PokeyMAX HW API ---- */

typedef struct {
    uint16_t period_by_tick[64];
    uint8_t  vol_by_tick[64];
    uint8_t  wrote_tick[64];
} Trace;

static Trace g_trace;
static uint8_t g_tick_for_trace;

static void trace_write(uint16_t period, uint8_t vol)
{
    uint8_t t = g_tick_for_trace;
    g_trace.period_by_tick[t] = period;
    g_trace.vol_by_tick[t] = vol;
    g_trace.wrote_tick[t] = 1;
}

void pokeymax_channel_setup(uint8_t chan, uint16_t addr, uint16_t len,
                           uint16_t period, uint8_t vol,
                           uint8_t is_8bit, uint8_t is_adpcm)
{
    (void)chan; (void)addr; (void)len; (void)is_8bit; (void)is_adpcm;
    trace_write(period, vol);
}

void pokeymax_channel_set_period_vol(uint8_t chan, uint16_t period, uint8_t vol)
{
    (void)chan;
    trace_write(period, vol);
}

/* Unused in this test but required by link on some builds */
void pokeymax_channel_dma_on(uint8_t chan) { (void)chan; }
void pokeymax_channel_dma_off(uint8_t chan) { (void)chan; }
void pokeymax_enable_irqs(uint8_t mask) { (void)mask; }
void pokeymax_disable_irqs(uint8_t mask) { (void)mask; }

/* Shim PEEK/POKE backends (not used by this test, but modplayer links them). */
uint8_t pokeymax_mock_peek(uint16_t addr) { (void)addr; return 0; }
void    pokeymax_mock_poke(uint16_t addr, uint8_t val) { (void)addr; (void)val; }

/* Pattern/prefetch stubs (not used by this unit test). */
uint8_t mod_get_row_ptr(uint8_t pat, uint8_t row, const uint8_t **out) {
    (void)pat; (void)row; (void)out; return 1; }
void mod_pattern_advance(uint8_t cur_pat, uint8_t next_pat) {
    (void)cur_pat; (void)next_pat; }

/* ---- Helpers ---- */

static void clear_trace(void)
{
    memset(&g_trace, 0, sizeof(g_trace));
}

static void encode_note(uint8_t *dst4, uint8_t sample, uint16_t period,
                        uint8_t effect, uint8_t param)
{
    /* MOD packed note:
       b0: sample_hi (4) | period_hi (4)
       b1: period_lo
       b2: sample_lo (4) | effect (4)
       b3: param
     */
    uint8_t b0 = (uint8_t)((sample & 0xF0u) | ((period >> 8) & 0x0Fu));
    uint8_t b1 = (uint8_t)(period & 0xFFu);
    uint8_t b2 = (uint8_t)(((sample & 0x0Fu) << 4) | (effect & 0x0Fu));
    uint8_t b3 = param;
    dst4[0]=b0; dst4[1]=b1; dst4[2]=b2; dst4[3]=b3;
}

static void setup_one_sample(uint8_t ds_factor, uint8_t is_adpcm)
{
    /* sample 1 */
    mod.samples[1].length = 1000;
    mod.samples[1].pokeymax_len = 1000;
    mod.samples[1].pokeymax_addr = 0;
    mod.samples[1].volume = 64;
    mod.samples[1].finetune = 0;
    mod.samples[1].downsample_factor = ds_factor;
    mod.samples[1].is_adpcm = is_adpcm;
    mod.samples[1].is_8bit = 1;
    mod.samples[1].has_loop = 0;
    mod.samples[1].loop_start = 0;
    mod.samples[1].loop_len = 0;
}

static Trace run_case(uint8_t ds_factor, uint8_t is_adpcm,
                      uint8_t effect, uint8_t param,
                      uint16_t period)
{
    uint8_t row[16];
    memset(row, 0, sizeof(row));

    mod_test_reset_state();
    setup_one_sample(ds_factor, is_adpcm);

    clear_trace();
    g_tick_for_trace = 0;

    /* note on channel 1 */
    encode_note(&row[0], 1, period, effect, param);
    /* other channels empty */

    mod_test_apply_row_bytes(row);

    /* ticks 1..5 */
    for (uint8_t t = 1; t < 6; t++) {
        g_tick_for_trace = t;
        mod_test_set_tick(t);
        mod_test_update_effects();
    }

    return g_trace;
}

static int expect_scaled(const Trace *base, const Trace *scaled, uint8_t factor,
                         const char *label)
{
    for (uint8_t t = 0; t < 6; t++) {
        if (base->wrote_tick[t] != scaled->wrote_tick[t]) {
            fprintf(stderr, "%s: tick %u write mismatch (base=%u scaled=%u)\n",
                    label, (unsigned)t, base->wrote_tick[t], scaled->wrote_tick[t]);
            return 0;
        }
        if (!base->wrote_tick[t]) continue;
        uint32_t want = (uint32_t)base->period_by_tick[t] * (uint32_t)factor;
        if (want > 0xFFFFu) want = 0xFFFFu;
        if (scaled->period_by_tick[t] != (uint16_t)want) {
            fprintf(stderr,
                    "%s: tick %u period mismatch base=%u factor=%u want=%u got=%u\n",
                    label, (unsigned)t,
                    (unsigned)base->period_by_tick[t], (unsigned)factor,
                    (unsigned)want, (unsigned)scaled->period_by_tick[t]);
            return 0;
        }
    }
    return 1;
}

static int expect_equal(const Trace *a, const Trace *b, const char *label)
{
    for (uint8_t t = 0; t < 6; t++) {
        if (a->wrote_tick[t] != b->wrote_tick[t]) {
            fprintf(stderr, "%s: tick %u write mismatch (a=%u b=%u)\n",
                    label, (unsigned)t, a->wrote_tick[t], b->wrote_tick[t]);
            return 0;
        }
        if (!a->wrote_tick[t]) continue;
        if (a->period_by_tick[t] != b->period_by_tick[t]) {
            fprintf(stderr, "%s: tick %u period mismatch a=%u b=%u\n",
                    label, (unsigned)t,
                    (unsigned)a->period_by_tick[t], (unsigned)b->period_by_tick[t]);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    struct {
        uint8_t fx;
        uint8_t param;
        const char *name;
    } cases[] = {
        { FX_ARPEGGIO,       0x37, "arpeggio" },
        { FX_SLIDE_UP,       0x02, "slide_up" },
        { FX_SLIDE_DOWN,     0x02, "slide_down" },
        { FX_TONE_PORTAMENTO,0x04, "tone_porta" },
        { FX_VIBRATO,        0x47, "vibrato" },
    };

    const uint16_t period = 428; /* ~C-3 in PT */

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        Trace pcm1 = run_case(1, 0, cases[i].fx, cases[i].param, period);
        Trace pcm2 = run_case(2, 0, cases[i].fx, cases[i].param, period);
        Trace adp1 = run_case(1, 1, cases[i].fx, cases[i].param, period);
        Trace adp2 = run_case(2, 1, cases[i].fx, cases[i].param, period);

        if (!expect_scaled(&pcm1, &pcm2, 2, cases[i].name)) return 1;
        if (!expect_equal(&pcm1, &adp1, cases[i].name)) return 1;
        if (!expect_equal(&pcm2, &adp2, cases[i].name)) return 1;
    }

    printf("test13_effect_format_parity_linux: PASS\n");
    return 0;
}
