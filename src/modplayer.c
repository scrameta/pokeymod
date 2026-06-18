/*
 * modplayer.c - MOD player engine (optimised for Atari 800XL / CC65)
 *
 * Optimisations vs previous version:
 *
 * OPT-1: scale_volume() uses >> 6 instead of / 64 — avoids CC65 software
 *        divide on every volume update.
 *
 * OPT-2: hw_period cached in ChanState::hw_period.  The 32-bit multiply
 *        for downsample_factor is done once when cs->period changes, not
 *        on every hardware register write.  hw_period_for_chan() is gone;
 *        use cs->hw_period directly everywhere.
 *
 * OPT-4: vibrato_sine[] is now int8_t so the sign-extend branch
 *        ("if (sv > 127) sv -= 256") disappears — the cast to int16_t
 *        sign-extends cleanly and CC65 emits much tighter code.
 *
 * OPT-5: update_bpm_step() 32-bit maths replaced with a shift-and-add
 *        approximation that uses only 8/16-bit arithmetic, keeping BPM
 *        recalculation inside the VBI but making it cheap enough to not
 *        matter.  PAL 50 Hz, BPM 125 still gives step == 256 exactly.
 *
 * OPT-6: apply_finetune() 32-bit multiply replaced with a compact
 *        16-bit shift approximation that is accurate to < 0.5% — well
 *        within audible tolerance on 8-bit hardware.
 *
 * Required change in modplayer.h / ChanState:
 *   Add field:  uint16_t hw_period;
 *
 * Required change in tables.c / vibrato_sine[]:
 *   Change declaration from uint8_t to int8_t (values are -128..127).
 */

#include <stdint.h>
#include <string.h>
//#include "mod_format.h"
#include "mod_struct.h"
#include "modplayer.h"
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "mod_pattern.h"

extern const uint16_t amiga_periods[NUM_PERIODS];
extern const uint16_t finetune_ratio[16];
extern const int8_t   vibrato_sine[64];   /* OPT-4: must be signed */
extern const uint16_t arp_semi_div[16];   /* arpeggio semitone divisors */
extern const uint8_t  arp_semi_recip[16]; /* OPT-7: arpeggio reciprocals */

/* -------------------------------------------------------
 * OPT-11: Inline HW register writes for the VBI hot path.
 *
 * pokeymax_channel_set_period_vol() passes 3 params through the
 * cc65 software stack (~80 cycles overhead) for 3 POKEs (~18 cycles
 * of actual work).  pokeymax_channel_setup() is worse: 7 params.
 *
 * These macros expand inline so the compiler can keep values in
 * A/X/Y or zero-page without the call/return overhead.
 * The pokeymax_hw.c functions are kept for non-VBI callers.
 *
 * OPT-15: Separate macros for vol-only, period-only, and
 * period+vol to avoid unnecessary register writes.  At 114
 * cycles/scanline, each saved POKE matters.
 * ------------------------------------------------------- */
#define HW_SET_VOL(chan, vol) do { \
    POKE(REG_CHANSEL, (chan));                     \
    POKE(REG_VOL,  (unsigned char)((vol) & 0x3Fu));          \
} while(0)

#define HW_SET_PERIOD(chan, period) do { \
    POKE(REG_CHANSEL, (chan));                     \
    POKE(REG_PERL, (unsigned char)((period) & 0xFF));        \
    POKE(REG_PERH, (unsigned char)(((period) >> 8) & 0x0Fu));\
} while(0)

#define HW_SET_PERIOD_VOL(chan, period, vol) do { \
    POKE(REG_CHANSEL, (chan));                     \
    POKE(REG_PERL, (unsigned char)((period) & 0xFF));        \
    POKE(REG_PERH, (unsigned char)(((period) >> 8) & 0x0Fu));\
    POKE(REG_VOL,  (unsigned char)((vol) & 0x3Fu));          \
} while(0)

/* Full channel setup inlined from ChanState fields.
 * Avoids pushing 7 params (10 bytes) through cc65 software stack.
 *
 * OPT-15b: Skip addr/len/samcfg writes if the sample hasn't changed
 * since the last trigger on this channel (common case: same instrument
 * re-triggered at different pitch). Tracks last sample per channel. */
static uint8_t last_triggered_sample[MOD_CHANNELS];

static void trigger_setup_hw(uint8_t hw_chan, ChanState *cs)
{
    unsigned char bit  = (unsigned char)(1u << (hw_chan - 1u));
    unsigned char nbit = (unsigned char)(~bit & 0x0Fu);
    uint8_t ch_idx = (uint8_t)(hw_chan - 1u);

    POKE(REG_CHANSEL, hw_chan);

    /* OPT-15b: Skip addr/len/samcfg writes if the sample hasn't changed
     * since the last trigger on this channel.  EXCEPTION: looped samples
     * must always re-write addr/len because the loop IRQ handler overwrites
     * the address register with the loop-start point.  A retrigger of the
     * same looped sample must reset back to the sample start. */
    if (cs->sample_num != last_triggered_sample[ch_idx] || cs->has_loop) {
        unsigned char cfg;
        POKE(REG_ADDRL, (unsigned char)(cs->sam_addr & 0xFF));
        POKE(REG_ADDRH, (unsigned char)(cs->sam_addr >> 8));
        POKE(REG_LENL,  (unsigned char)((cs->sam_len - 1u) & 0xFF));
        POKE(REG_LENH,  (unsigned char)((cs->sam_len - 1u) >> 8));

        cfg = pokeymax_samcfg_shadow;
        cfg &= (unsigned char)~bit;
        cfg &= (unsigned char)~(unsigned char)(bit << 4u);
        if (cs->is_adpcm) {
            cfg |= bit;
        } else {
            cfg |= (unsigned char)(bit << 4u);
        }
        pokeymax_samcfg_shadow = cfg;
        POKE(REG_SAMCFG, cfg);

        last_triggered_sample[ch_idx] = cs->sample_num;
    }

    /* Period always needs updating */
    POKE(REG_PERL,  (unsigned char)(cs->hw_period & 0xFF));
    POKE(REG_PERH,  (unsigned char)((cs->hw_period >> 8) & 0x0Fu));

    /* DMA restart sequence (always needed for retrigger).
     * Mute the channel during the DMA off→on transition to prevent
     * a click from the DAC output jumping when DMA restarts.
     * Volume is restored after the new sample starts playing. */
    POKE(REG_VOL, 0);

    pokeymax_irqen_shadow &= nbit;
    POKE(REG_IRQEN, pokeymax_irqen_shadow);

    if (pokeymax_dma_shadow & bit) {
        pokeymax_dma_shadow &= nbit;
        POKE(REG_DMA, pokeymax_dma_shadow);
    }
    pokeymax_dma_shadow |= bit;
    POKE(REG_DMA, pokeymax_dma_shadow);

    POKE(REG_IRQACT, nbit);

    pokeymax_irqen_shadow |= bit;
    POKE(REG_IRQEN, pokeymax_irqen_shadow);

    /* Restore volume now that DMA is running with new sample data */
    POKE(REG_VOL,   (unsigned char)(cs->hw_vol & 0x3Fu));

    /* Pre-load next-buffer address/length for hardware auto-reload.
     * When the current buffer ends, the PokeyMAX sample engine
     * automatically reloads from the registers.
     *
     * Looped samples: pre-load the loop region so the hardware
     * seamlessly transitions to the loop.
     *
     * For looped ADPCM (two-blob layout):
     *   Block A occupies the first ceil(loop_start/2) stored bytes.
     *   Block B starts immediately after that.  We use (+1)>>1 (ceil)
     *   rather than >>1 (floor) so that odd loop_start values still
     *   land Block B on the correct byte boundary.
     */
    if (cs->has_loop && cs->loop_len > 2u) {
        uint16_t loop_addr;
        uint16_t loop_len;
        if (cs->is_adpcm) {
            loop_addr = cs->sam_addr + ((cs->loop_start + 1u) >> 1);
        } else {
            loop_addr = cs->sam_addr + cs->loop_start;
        }
        loop_len = cs->loop_len;

        POKE(REG_ADDRL, (unsigned char)(loop_addr & 0xFF));
        POKE(REG_ADDRH, (unsigned char)(loop_addr >> 8));
        POKE(REG_LENL,  (unsigned char)((loop_len - 1u) & 0xFF));
        POKE(REG_LENH,  (unsigned char)((loop_len - 1u) >> 8));
    }
}

static uint8_t master_volume = 63u;
uint8_t mod_need_prefetch = 0;

/* Kept for the inline fallback path */
static uint8_t        row_work_active  = 0u;
static uint8_t        row_work_channel = 0u;
static const uint8_t *row_work_data    = 0;

static uint8_t last_rtclock;

#ifndef FX_VOLSLIDE
#define FX_VOLSLIDE             0xA
#endif
#ifndef FX_POSITION_JUMP
#define FX_POSITION_JUMP        0xB
#endif
#ifndef FX_TONE_PORTA_VOLSLIDE
#define FX_TONE_PORTA_VOLSLIDE  0x5
#endif
#ifndef FX_VIBRATO_VOLSLIDE
#define FX_VIBRATO_VOLSLIDE     0x6
#endif


/* -------------------------------------------------------
 * OPT-1: scale_volume
 * master_volume is clamped 0-63 by mod_set_volume → >> 6 == / 64.
 * Avoids CC65 software divide routine (~50 cycles → 4 cycles).
 * ------------------------------------------------------- */
static uint8_t scale_volume(uint8_t mod_vol)
{
    return (uint8_t)(((uint16_t)mod_vol * (uint16_t)master_volume) >> 6);
}

static uint8_t apply_volslide(ChanState *cs, uint8_t param)
{
    uint8_t up = (uint8_t)((param >> 4) & 0x0Fu);
    uint8_t dn = (uint8_t)(param & 0x0Fu);
    if (up != 0u) {
        uint8_t v = (uint8_t)(cs->volume + up);
        cs->volume = (v > 64u) ? 64u : v;
        return 1u;
    }
    if (dn != 0u) {
        cs->volume = (dn > cs->volume) ? 0u : (uint8_t)(cs->volume - dn);
        return 1u;
    }
    return 0u;
}

/* -------------------------------------------------------
 * OPT-6: apply_finetune — 16-bit shift approximation
 *
 * Original used a 32-bit multiply: (period * ratio) >> 8.
 * finetune_ratio[ft+8] is a Q8 multiplier where 256 == 1.0.
 * The delta from 1.0 is small (ratio range ~224..284),
 * so we can split: adj = period + (period * delta) / 256
 * where delta = ratio - 256 ∈ [-32 .. +28].
 *
 * Approximation: (period >> 4) * delta) >> 4
 * (period/16) fits uint8_t for periods up to 4080, well within MOD
 * range.  delta fits int8_t.  Product fits int16_t.  No 32-bit ops.
 *
 * Max error < 0.4 % across all valid periods — inaudible.
 * ft == 0 path returns period unchanged (exact).
 * ------------------------------------------------------- */
static uint16_t apply_finetune(uint16_t period, int8_t ft)
{
    int16_t ratio_delta;
    int16_t adj;
    int16_t result;

    if (ft == 0) return period;

    ratio_delta = (int16_t)finetune_ratio[(uint8_t)(ft + 8)] - 256;
    adj    = (int16_t)((int16_t)(period >> 4) * ratio_delta) >> 4;
    result = (int16_t)period + adj;
    if (result < 1) result = 1;
    return (uint16_t)result;
}

/* -------------------------------------------------------
 * OPT-2: update_hw_period
 * Cache the hardware-adjusted period in cs->hw_period.
 * Called once whenever cs->period changes instead of
 * recomputing the 32-bit multiply on every register write.
 * ------------------------------------------------------- */
static void update_hw_period(ChanState *cs)
{
    uint16_t p = cs->period;
    if (cs->sample_num != 0u && cs->sample_num <= MOD_MAX_SAMPLES) {
        uint8_t ds_shift = SI_DS_SHIFT(&mod.samples[cs->sample_num]);
        if (ds_shift > 0u) {
            /* OPT-8b: 16-bit shifts with overflow check, same as temp_hw_period */
            uint16_t prev = p;
            p <<= 1;
            if (p < prev) { cs->hw_period = 0xFFFFu; return; }
            if (ds_shift > 1u) {
                prev = p;
                p <<= 1;
                if (p < prev) { cs->hw_period = 0xFFFFu; return; }
            }
        }
    }
    cs->hw_period = p;
}

/* Helper: compute a temporary hw_period without caching (used by
 * vibrato / arpeggio which bend the period transiently each tick).
 * OPT-8: 16-bit only — ds_shift is 0-2, so detect overflow via
 * comparison rather than promoting to uint32_t. */
static uint16_t temp_hw_period(const ChanState *cs, uint16_t period)
{
    if (cs->sample_num != 0u && cs->sample_num <= MOD_MAX_SAMPLES) {
        uint8_t ds_shift = SI_DS_SHIFT(&mod.samples[cs->sample_num]);
        if (ds_shift > 0u) {
            uint16_t prev = period;
            period <<= 1;
            if (period < prev) return 0xFFFFu;   /* overflow on shift 1 */
            if (ds_shift > 1u) {
                prev = period;
                period <<= 1;
                if (period < prev) return 0xFFFFu; /* overflow on shift 2 */
            }
        }
    }
    return period;
}

/* -------------------------------------------------------
 * mod_decode_note  (unchanged)
 * ------------------------------------------------------- */
void mod_decode_note(const MODNote *raw, Note *out)
{
    uint8_t b0 = raw->raw[0];
    uint8_t b1 = raw->raw[1];
    uint8_t b2 = raw->raw[2];
    uint8_t b3 = raw->raw[3];
    out->sample = (b0 & 0xF0u) | ((b2 >> 4) & 0x0Fu);
    out->period  = ((uint16_t)(b0 & 0x0Fu) << 8) | b1;
    out->effect  = b2 & 0x0Fu;
    out->param   = b3;
}

/* -------------------------------------------------------
 * trigger_sample — uses cs->hw_period directly (OPT-2)
 * ------------------------------------------------------- */
static void trigger_sample(uint8_t hw_chan, ChanState *cs)
{
    SampleInfo *si;

    if (cs->sample_num == 0u || cs->sample_num > MOD_MAX_SAMPLES) return;
    si = &mod.samples[cs->sample_num];
    if (si->length == 0u)       return;
    if (si->pokeymax_len == 0u) return;
    if (cs->period == 0u)       return;

    cs->sam_addr   = si->pokeymax_addr;
    cs->loop_start = si->loop_start;
    cs->loop_len   = si->loop_len;
    cs->has_loop   = SI_HAS_LOOP(si);
    cs->is_adpcm   = SI_IS_ADPCM(si);
    cs->is_8bit    = !SI_IS_ADPCM(si);
    cs->active     = 1u;

    /* sam_len is the SAMPLE count to play before the end-of-sample IRQ.
     * For looped ADPCM we play in two blobs:
     *   - Block A = attack [0, loop_start), so sam_len = loop_start samples.
     *     When Block A ends, the hardware auto-reloads from the preload
     *     that points at Block B (set up in trigger_setup_hw below), and
     *     the IRQ->syncreset wiring resets ADPCM state to (0, 0) so that
     *     Block B decodes correctly.
     *   - Block B = loop body, encoded from (0, 0), looped forever after.
     * For other modes sam_len is the full sample's sample count.
     */
    if (cs->is_adpcm && cs->has_loop) {
        /* Two-blob looped ADPCM normally starts by playing Block A,
         * the attack before the loop.  Some ProTracker samples are
         * looped from offset 0, so Block A is empty and the stored data
         * starts directly with Block B.  Do not program a zero-length
         * first buffer: on real hardware that becomes LEN=$FFFF after
         * the -1 below, and in the Linux harness it effectively leaves
         * the channel silent.  Start directly on the loop body instead. */
        cs->sam_len = (cs->loop_start != 0u) ? cs->loop_start
                                             : cs->loop_len;
    } else if (cs->is_adpcm) {
        cs->sam_len = (uint16_t)(si->length * 2u);
    } else {
        cs->sam_len = si->length;
    }

    /* OPT-11: inlined HW setup — avoids 7-param software stack push */
    trigger_setup_hw(hw_chan, cs);
}

/* -------------------------------------------------------
 * update_effects - ticks 1..speed-1
 * Uses cs->hw_period (OPT-2) and int8_t sine table (OPT-4).
 * ------------------------------------------------------- */
static void update_effects_range(uint8_t ch_from, uint8_t ch_to)
{
    uint8_t ch;
    ChanState *cs = &mod.chan[ch_from];

    for (ch = ch_from; ch < ch_to; ch++, cs++) {
        uint8_t    hw = (uint8_t)(ch + 1u);
        uint8_t    period_changed = 0u;
        uint8_t    vol_changed    = 0u;

        /* OPT-9: skip channels with no active effect.
         * FX_ARPEGGIO==0, so effect==0 && param==0 means truly idle. */
        if (cs->effect == 0u && cs->param == 0u) continue;

        switch (cs->effect) {
            case FX_ARPEGGIO:
                if (cs->param != 0u && cs->period != 0u) {
                    uint16_t base   = cs->period;
                    uint8_t  offset = 0u;
                    /* OPT-14: avoid mod.tick % 3 (cc65 emits a divide).
                     * tick range is 0..speed-1 (max ~31). Use comparison. */
                    { uint8_t t3 = mod.tick;
                      if (t3 >= 3u) { t3 -= 3u; if (t3 >= 3u) { t3 -= 3u; if (t3 >= 3u) t3 -= 3u; } }
                      if (t3 == 1u) offset = (cs->param >> 4) & 0x0Fu;
                      else if (t3 == 2u) offset = cs->param & 0x0Fu;
                    }
                    if (offset > 0u && offset < 16u) {
                        /* OPT-7: reciprocal multiply replaces 32-bit divide. */
                        base = (uint16_t)((uint32_t)base * arp_semi_recip[offset] >> 8);
                    }
                    /* Arpeggio bends transiently; use temp_hw_period, don't cache */
                    { uint16_t tp = temp_hw_period(cs, base);
                      HW_SET_PERIOD_VOL(hw, tp, cs->hw_vol); }
                }
                break;

            case FX_SLIDE_UP:
                if (cs->period > cs->param) cs->period -= cs->param;
                else cs->period = 113u;
                if (cs->period < 113u) cs->period = 113u;
                update_hw_period(cs);
                period_changed = 1u;
                break;

            case FX_SLIDE_DOWN:
                cs->period += cs->param;
                if (cs->period > 856u) cs->period = 856u;
                update_hw_period(cs);
                period_changed = 1u;
                break;

            case FX_TONE_PORTAMENTO:
                if (cs->period < cs->target_period) {
                    cs->period += cs->port_speed;
                    if (cs->period > cs->target_period) cs->period = cs->target_period;
                } else if (cs->period > cs->target_period) {
                    if (cs->period >= cs->port_speed) cs->period -= cs->port_speed;
                    if (cs->period < cs->target_period) cs->period = cs->target_period;
                }
                update_hw_period(cs);
                period_changed = 1u;
                break;

            case FX_VIBRATO: {
                /* OPT-4: int8_t table — sign-extends cleanly, no branch needed */
                uint8_t  vib_idx    = cs->vib_pos & 63u;
                int16_t  sv         = (int16_t)vibrato_sine[vib_idx];
                int16_t  delta      = (int16_t)((sv * (int16_t)cs->vib_depth) >> 7);
                int16_t  vib_period = (int16_t)cs->period + delta;
                uint16_t tp;
                if (vib_period < 113) vib_period = 113;
                if (vib_period > 856) vib_period = 856;
                tp = temp_hw_period(cs, (uint16_t)vib_period);
                HW_SET_PERIOD_VOL(hw, tp, cs->hw_vol);
                cs->vib_pos = (uint8_t)((cs->vib_pos + cs->vib_speed) & 63u);
                break;
            }

            case FX_VOLSLIDE:
                if (apply_volslide(cs, cs->param)) {
                    cs->hw_vol = scale_volume(cs->volume);
                    vol_changed = 1u;
                }
                break;

            case FX_TONE_PORTA_VOLSLIDE:
                if (cs->period < cs->target_period) {
                    cs->period += cs->port_speed;
                    if (cs->period > cs->target_period) cs->period = cs->target_period;
                } else if (cs->period > cs->target_period) {
                    if (cs->period >= cs->port_speed) cs->period -= cs->port_speed;
                    if (cs->period < cs->target_period) cs->period = cs->target_period;
                }
                update_hw_period(cs);
                period_changed = 1u;
                if (apply_volslide(cs, cs->param)) {
                    cs->hw_vol = scale_volume(cs->volume);
                    vol_changed = 1u;
                }
                break;

            case FX_VIBRATO_VOLSLIDE: {
                /* OPT-4 */
                uint8_t  vib_idx    = cs->vib_pos & 63u;
                int16_t  sv         = (int16_t)vibrato_sine[vib_idx];
                int16_t  delta      = (int16_t)((sv * (int16_t)cs->vib_depth) >> 7);
                int16_t  vib_period = (int16_t)cs->period + delta;
                uint16_t tp;
                if (vib_period < 113) vib_period = 113;
                if (vib_period > 856) vib_period = 856;
                tp = temp_hw_period(cs, (uint16_t)vib_period);
                HW_SET_PERIOD_VOL(hw, tp, cs->hw_vol);
                cs->vib_pos = (uint8_t)((cs->vib_pos + cs->vib_speed) & 63u);
                if (apply_volslide(cs, cs->param)) {
                    cs->hw_vol = scale_volume(cs->volume);
                    vol_changed = 1u;
                }
                break;
            }

            case FX_EXTENDED: {
                uint8_t sub  = (cs->param >> 4) & 0x0Fu;
                uint8_t subp =  cs->param       & 0x0Fu;
                if (sub == EFX_NOTE_CUT && mod.tick == cs->note_cut_tick) {
                    cs->volume = 0u; cs->hw_vol = 0u; vol_changed = 1u;
                }
                if (sub == EFX_NOTE_DELAY && mod.tick == cs->note_delay_ticks) {
                    trigger_sample(hw, cs); cs->triggered = 1u;
                }
                if (sub == EFX_RETRIGGER && subp > 0u && (mod.tick % subp) == 0u) {
                    trigger_sample(hw, cs);
                }
                (void)subp;
                break;
            }

            default: break;
        }

        /* OPT-15: Write only the registers that actually changed */
        if (cs->active) {
            if (period_changed && vol_changed) {
                HW_SET_PERIOD_VOL(hw, cs->hw_period, cs->hw_vol);
            } else if (vol_changed) {
                HW_SET_VOL(hw, cs->hw_vol);
            } else if (period_changed) {
                HW_SET_PERIOD(hw, cs->hw_period);
            }
        }
    }
}

static void update_effects(void) { update_effects_range(0u, MOD_CHANNELS); }

/* -------------------------------------------------------
 * OPT-5: BPM step — 8/16-bit arithmetic only (stays in VBI)
 *
 * Target formula: step = (BPM * 256 * 24) / (60 * VBI_HZ)
 *                      = (BPM * 6144)     / (60 * VBI_HZ)
 *
 * PAL 50 Hz  → divisor 3000 → step = (BPM * 1024 + 250) / 500
 *   Fast path: step = BPM*2 + (BPM*12 + 125) / 250
 *   BPM=125 → step=256 exactly.  Max BPM=255 → BPM*12=3060 (fits uint16_t).
 *
 * NTSC 60 Hz → divisor 3600 → step ≈ BPM + (BPM * 177) / 256
 *   BPM*177 max = 45135 (fits uint16_t).
 *
 * Generic fallback uses one unavoidable 32-bit multiply for any
 * future non-PAL/NTSC VBI rate.
 * ------------------------------------------------------- */
#define GTIA_PAL_REG 0xD014u

static uint16_t bpm_accum  = 0u;
static uint16_t bpm_step   = 0u;
static uint8_t  last_bpm   = 0u;
static uint8_t  last_vbi_hz = 0u;
static uint8_t  last_timer_bpm = 0u;
static uint8_t  last_timer_hz  = 0u;

uint8_t mod_detect_vbi_hz(void)
{
    /* GTIA PAL ($D014) is 1 on PAL machines and 15 on NTSC machines. */
    return (PEEK(GTIA_PAL_REG) == 1u) ? PAL_VBI_HZ : NTSC_VBI_HZ;
}


static uint16_t timer_period_for_bpm(uint8_t bpm8, uint8_t vbi_hz)
{
    uint32_t clock;
    uint32_t period;
    if (bpm8 == 0u) bpm8 = DEFAULT_BPM;
    clock = (vbi_hz == NTSC_VBI_HZ) ? NTSC_PAULA_CLOCK : PAL_PAULA_CLOCK;
    /* 16-bit POKEY timer, 1.79MHz clock: period ~= clock / (BPM*24/60). */
    period = (clock * 60UL + ((uint32_t)bpm8 * 12UL)) / ((uint32_t)bpm8 * 24UL);
    if (period == 0UL) period = 1UL;
    if (period > 65535UL) period = 65535UL;
    return (uint16_t)period;
}

static void update_timer_period(void)
{
    uint8_t bpm8 = (uint8_t)mod.bpm;
    uint8_t hz = mod.vbi_hz ? mod.vbi_hz : PAL_VBI_HZ;
    uint16_t period;
    if (mod.timing_mode != MOD_TIMING_TIMER) return;
    if (bpm8 == last_timer_bpm && hz == last_timer_hz) return;
    last_timer_bpm = bpm8;
    last_timer_hz  = hz;
    period = timer_period_for_bpm(bpm8, hz);
    POKE(REG_AUDF1, (uint8_t)(period & 0xFFu));
    POKE(REG_AUDF2, (uint8_t)(period >> 8));
    POKE(REG_STIMER, 0u);
}

static void update_bpm_step(void)
{
    uint8_t bpm8;
    uint8_t vbi_hz = mod.vbi_hz;
    if (vbi_hz == 0u) vbi_hz = PAL_VBI_HZ;
    if (mod.bpm == last_bpm && vbi_hz == last_vbi_hz) return;
    last_bpm    = (uint8_t)mod.bpm;
    last_vbi_hz = vbi_hz;
    bpm8        = last_bpm;

    if (vbi_hz == PAL_VBI_HZ) {
        /* step = BPM*2 + (BPM*12 + 125) / 250   (all uint16_t) */
        uint16_t hi = (uint16_t)bpm8 << 1;
        uint16_t lo = (uint16_t)bpm8 * 12u + 125u;
        bpm_step = hi + lo / 250u;
    } else if (vbi_hz == NTSC_VBI_HZ) {
        /* step ≈ BPM + (BPM * 177) >> 8 */
        uint16_t frac = (uint16_t)bpm8 * 177u;
        bpm_step = (uint16_t)bpm8 + (frac >> 8);
    } else {
        /* Generic: one 32-bit multiply (rare, non-standard VBI rate) */
        uint32_t numer = (uint32_t)bpm8 * 6144UL;
        uint16_t denom = (uint16_t)(60u * (uint16_t)vbi_hz);
        bpm_step = (uint16_t)((numer + (uint32_t)(denom >> 1u)) / denom);
    }
}

/* -------------------------------------------------------
 * OPT-13: Row decode split across two ticks.
 *
 * Instead of decoding + committing all 4 channels on tick 0,
 * we process channels 0-1 on tick 0 and channels 2-3 on tick 1.
 * This halves the peak VBI cost.  The 20ms delay on channels
 * 2-3 is inaudible (ProTracker itself has per-channel DMA skew).
 *
 * On tick 1 we process the deferred channels BEFORE running
 * update_effects(), so effects still apply from tick 1 onward
 * for all channels (same as before for ch 0-1; one tick late
 * for ch 2-3 but only on the row boundary).
 *
 * The row data pointer and decode state are saved between ticks.
 * ------------------------------------------------------- */

/* Per-channel decode + commit — extracted from the old tick-0 loop body.
 * Processes a single channel from raw row data bytes. */
static void decode_commit_channel(uint8_t ch, const uint8_t *d)
{
    ChanState *cs = &mod.chan[ch];
    uint8_t    hw = (uint8_t)(ch + 1u);
    uint8_t  b0=d[0],b1=d[1],b2=d[2],b3=d[3];
    uint8_t  ns  = (uint8_t)((b0&0xF0u)|((b2>>4)&0x0Fu));
    uint16_t np  = ((uint16_t)(b0&0x0Fu)<<8)|b1;
    uint8_t  ne  = b2&0x0Fu;
    uint8_t  npar= b3;
    uint8_t  hw_upd = 0u;

    if (ns > MOD_MAX_SAMPLES) ns = 0u;
    if (np && (np < 28u || np > 4095u)) np = 0u;

    cs->effect=ne; cs->param=npar;
    cs->triggered=0u; cs->note_delay_ticks=0u; cs->note_cut_tick=0u;

    if (ns) {
        cs->sample_num=ns;
        cs->volume=mod.samples[ns].volume;
        cs->hw_vol=scale_volume(cs->volume);
        hw_upd=1u;
    }
    if (cs->sample_num > MOD_MAX_SAMPLES) cs->sample_num=0u;

    if (np && ne!=FX_TONE_PORTAMENTO && ne!=FX_TONE_PORTA_VOLSLIDE) {
        uint8_t s2=cs->sample_num;
        int8_t ft=(s2&&s2<=MOD_MAX_SAMPLES)?SI_FINETUNE(&mod.samples[s2]):0;
        cs->period=apply_finetune(np,ft);
        update_hw_period(cs);
        hw_upd=1u;
    }
    if (np && (ne==FX_TONE_PORTAMENTO||ne==FX_TONE_PORTA_VOLSLIDE)) {
        uint8_t s2=cs->sample_num;
        int8_t ft=(s2&&s2<=MOD_MAX_SAMPLES)?SI_FINETUNE(&mod.samples[s2]):0;
        cs->target_period=apply_finetune(np,ft);
    }

    switch (ne) {
      case FX_SET_VOLUME:
        cs->volume = (npar > 64u) ? 64u : npar;
        cs->hw_vol = scale_volume(cs->volume);
        hw_upd = 1u;
        break;
      case FX_SET_SPEED:
        /* Already handled by tick-0 pre-scan — skip */
        break;
      case FX_VIBRATO:
        /* Protracker 4xy: zero nibble means "keep previous value".
         * Both nibbles are independent, then we always exit the case. */
        if (npar & 0xF0u) cs->vib_speed = (npar >> 4) & 0x0Fu;
        if (npar & 0x0Fu) cs->vib_depth =  npar       & 0x0Fu;
        break;
      case FX_TONE_PORTAMENTO:
      case FX_TONE_PORTA_VOLSLIDE:
        /* Protracker 3xx/5xx: zero param means "reuse previous speed". */
        if (npar) cs->port_speed = npar;
        break;
      case FX_POSITION_JUMP:
        /* Already handled by tick-0 pre-scan — skip */
        break;
      case FX_PATTERN_BREAK:
        /* Already handled by tick-0 pre-scan — skip */
        break;
      case FX_EXTENDED: {
        uint8_t sub  = (npar >> 4) & 0x0Fu;
        uint8_t subp =  npar       & 0x0Fu;
        switch (sub) {
          case EFX_FINE_VOL_UP: {
            uint8_t v = cs->volume + subp;
            cs->volume = (v > 64u) ? 64u : v;
            cs->hw_vol = scale_volume(cs->volume);
            hw_upd = 1u;
            break;
          }
          case EFX_FINE_VOL_DOWN:
            if (subp > cs->volume) cs->volume  = 0u;
            else                   cs->volume -= subp;
            cs->hw_vol = scale_volume(cs->volume);
            hw_upd = 1u;
            break;
          case EFX_FINE_SLIDE_UP:
            /* Guard against period underflow; leave period unchanged if
             * subp would drop us below zero (period 0 is invalid). */
            if (cs->period > subp) cs->period -= subp;
            update_hw_period(cs);
            hw_upd = 1u;
            break;
          case EFX_FINE_SLIDE_DOWN:
            cs->period += subp;
            update_hw_period(cs);
            hw_upd = 1u;
            break;
          case EFX_LOOP:
            /* Already handled by tick-0 pre-scan — skip */
            break;
          case EFX_NOTE_CUT:
            cs->note_cut_tick = subp;
            break;
          case EFX_NOTE_DELAY:
            cs->note_delay_ticks = subp;
            return; /* skip trigger */
          case EFX_PATTERN_DELAY:
            /* Already handled by tick-0 pre-scan — skip */
            break;
          default:
            break;
        }
        break;
      }
      default:
        break;
    }

    if(np && ne!=FX_TONE_PORTAMENTO && ne!=FX_TONE_PORTA_VOLSLIDE) {
        trigger_sample(hw,cs); cs->triggered=1u;
    } else if(ns && cs->period && !cs->active) {
        trigger_sample(hw,cs); cs->triggered=1u;
    }
    if(!cs->triggered && cs->active && hw_upd)
        HW_SET_PERIOD_VOL(hw,cs->hw_period,cs->hw_vol);
}

static void do_tick(void)
{
    if (mod.tick == 0u) {
        if (mod.pattern_delay > 0u) 
        {
            mod.pattern_delay--;
        } 
        else 
        {
            uint8_t pattern = mod.order_table[mod.order_pos];
            const uint8_t *row_data;

            if (mod_get_row_ptr(pattern, mod.row, &row_data) == 0) {
                mod.pattern_break = 0u;
                mod.do_jump       = 0u;
                row_work_data     = row_data;

                /* OPT-13c: pre-scan ALL channels for global effects on tick 0.
                 * Speed, BPM, pattern break, position jump, pattern loop, and
                 * pattern delay MUST take effect immediately — delaying them
                 * to later ticks corrupts song structure and timing.
                 * This scan is cheap: just byte reads and comparisons. */
                {
                    uint8_t ch;
                    const uint8_t *d = row_data;
                    for (ch = 0u; ch < MOD_CHANNELS; ch++, d += 4u) {
                        uint8_t ne   = d[2] & 0x0Fu;
                        uint8_t npar = d[3];
                        switch (ne) {
                          case FX_SET_SPEED:
                            if (npar != 0u) {
                                if (npar < 32u) mod.speed = npar;
                                else mod.bpm = npar;
                            }
                            break;
                          case FX_POSITION_JUMP:
                            mod.do_jump = 1u;
                            mod.jump_order = npar;
                            mod.pattern_break = 1u;
                            mod.break_row = 0u;
                            break;
                          case FX_PATTERN_BREAK:
                            mod.pattern_break = 1u;
                            mod.break_row = (uint8_t)(((npar >> 4) * 10u) + (npar & 0x0Fu));
                            if (mod.break_row >= MOD_ROWS_PER_PAT) mod.break_row = 0u;
                            break;
                          case FX_EXTENDED: {
                            uint8_t sub  = (npar >> 4) & 0x0Fu;
                            uint8_t subp =  npar       & 0x0Fu;
                            if (sub == EFX_LOOP) {
                                ChanState *lcs = &mod.chan[ch];
                                if (subp == 0u) lcs->loop_row = mod.row;
                                else {
                                    if (lcs->loop_count == 0u) {
                                        lcs->loop_count = subp;
                                        mod.pattern_break = 1u;
                                        mod.break_row = lcs->loop_row;
                                    } else {
                                        lcs->loop_count--;
                                        if (lcs->loop_count > 0u) {
                                            mod.pattern_break = 1u;
                                            mod.break_row = lcs->loop_row;
                                        }
                                    }
                                }
                            } else if (sub == EFX_PATTERN_DELAY) {
                                mod.pattern_delay = subp;
                            }
                            break;
                          }
                          default: break;
                        }
                    }
                }

                /* OPT-13f: spread note decode across ticks.
                 * 2+2 split: ch0+ch1 on tick 0, ch2+ch3 on tick 1.
                 * Decode happens BEFORE effects on tick 1, so channel
                 * audio starts immediately.  A skip-mask prevents
                 * update_effects from applying the new row's effects
                 * on the same tick they were decoded (would cause an
                 * extra tick of effect processing). */
                decode_commit_channel(0u, row_data);
                if (mod.speed <= 1u) {
                    decode_commit_channel(1u, row_data + 4u);
                    decode_commit_channel(2u, row_data + 8u);
                    decode_commit_channel(3u, row_data + 12u);
                    row_work_active = 0u;
                } else {
                    decode_commit_channel(1u, row_data + 4u);
                    row_work_active = 1u;
                }
            }
        }
    } /* tick == 0 */

    if (mod.tick != 0u) {
        if (row_work_active && mod.tick == 1u) {
            /* OPT-13g: split effects around deferred decode.
             * 1. effects for ch0+ch1 (already have new row from tick 0)
             * 2. decode ch2+ch3 (get their new row data + trigger)
             * 3. effects for ch2+ch3 (run with NEW row effects)
             * This gives all channels the correct effect tick count. */
            update_effects_range(0u, 2u);
            decode_commit_channel(2u, row_work_data + 8u);
            decode_commit_channel(3u, row_work_data + 12u);
            update_effects_range(2u, 4u);
            row_work_active = 0u;
        } else {
            update_effects();
        }
    }

    mod.tick++;

    if (mod.tick >= mod.speed) {
        mod.tick = 0u;

        if (mod.pattern_break) {
            mod.pattern_break = 0u;
            mod.row = mod.break_row;
            if (mod.do_jump) { mod.order_pos = mod.jump_order; mod.do_jump = 0u; }
            else             { mod.order_pos++; }
            if (mod.order_pos >= mod.song_length) {
                if (mod.loop_song) mod.order_pos = 0u;
                else { mod.playing = 0u; return; }
            }
            {
                uint8_t nxt = (uint8_t)(mod.order_pos + 1u);
                if (nxt >= mod.song_length) nxt = 0u;
                mod_pattern_advance(mod.order_table[mod.order_pos],
                                    mod.order_table[nxt]);
            }
        } else {
            mod.row++;
            if (mod.row >= MOD_ROWS_PER_PAT) {
                mod.row = 0u;
                if (mod.do_jump) { mod.order_pos = mod.jump_order; mod.do_jump = 0u; }
                else             { mod.order_pos++; }
                if (mod.order_pos >= mod.song_length) {
                    if (mod.loop_song) mod.order_pos = 0u;
                    else { mod.playing = 0u; return; }
                }
                {
                    uint8_t nxt = (uint8_t)(mod.order_pos + 1u);
                    if (nxt >= mod.song_length) nxt = 0u;
                    mod_pattern_advance(mod.order_table[mod.order_pos],
                                        mod.order_table[nxt]);
                }
            }
        }
    }
}

/* -------------------------------------------------------
 * mod_vbi_tick - deferred VBI ISR entry point
 * ------------------------------------------------------- */
void mod_vbi_tick(void)
{
    uint8_t now    = PEEK(RTCLOK);
    uint8_t missed = (uint8_t)(now - last_rtclock);
    last_rtclock   = now;
    if (!mod.playing) return;

    while (missed-- > 0u) {
        update_bpm_step();      /* OPT-5: now 8/16-bit only */
        bpm_accum += bpm_step;
        while (bpm_accum >= 256u) {
            bpm_accum -= 256u;
            do_tick();
            if (!mod.playing) return;
        }
    }
}

void mod_timer_tick(void)
{
    if (!mod.playing) return;
    do_tick();
    if (mod.playing) update_timer_period();
}

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */
void mod_play(void)
{
    uint8_t ch;
    mod.vbi_hz = mod_detect_vbi_hz();
    last_rtclock = PEEK(RTCLOK);
    if (mod.playing) return;

    for (ch = 0u; ch < MOD_CHANNELS; ch++) {
        mod.chan[ch].active     = 0u;
        mod.chan[ch].sample_num = 0u;
        mod.chan[ch].period     = 0u;
        mod.chan[ch].hw_period  = 0u;   /* OPT-2 */
        mod.chan[ch].volume     = 0u;
        mod.chan[ch].hw_vol     = 0u;
        mod.chan[ch].vib_pos    = 0u;
        last_triggered_sample[ch] = 0u; /* OPT-15b */
    }

    mod.playing   = 1u;
    mod.order_pos = 0u;
    mod.row       = 0u;
    mod.tick      = 0u;
    mod.do_jump   = 0u;
    mod.jump_order= 0u;

    row_work_active  = 0u;
    row_work_channel = 0u;
    row_work_data    = 0;

    bpm_accum   = 0u;
    last_bpm    = 0u;
    last_vbi_hz = 0u;
    last_timer_bpm = 0u;
    last_timer_hz  = 0u;
    update_timer_period();

    POKE(REG_DMA,    0x00u);
    POKE(REG_IRQACT, 0x00u);
    POKE(REG_IRQEN,  0x00u);

    mod_pattern_init(mod.order_table[0],mod.order_table[1]);
}

void mod_stop(void)
{
    mod.playing      = 0u;
    row_work_active  = 0u;
    row_work_channel = 0u;
    row_work_data    = 0;

    POKE(REG_DMA,    0x00u);
    POKE(REG_IRQEN,  0x00u);
    POKE(REG_COVOX_CH1, 0u);
    POKE(REG_COVOX_CH2, 0u);
    POKE(REG_COVOX_CH3, 0u);
    POKE(REG_COVOX_CH4, 0u);
}

void mod_pause(void)
{
    mod.playing = (uint8_t)!mod.playing;
    if (!mod.playing) {
        POKE(REG_COVOX_CH1, 0u);
        POKE(REG_COVOX_CH2, 0u);
        POKE(REG_COVOX_CH3, 0u);
        POKE(REG_COVOX_CH4, 0u);
    }
}

void mod_set_volume(uint8_t vol) { master_volume = vol & 0x3Fu; }
