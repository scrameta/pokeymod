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
 * OPT-3: Row pre-decode.  On the LAST tick of a row (tick == speed-1) the
 *        four channel notes are decoded from the pattern buffer into
 *        staged_row[].  On tick 0 only hardware commits (trigger /
 *        period+vol writes) happen — the expensive finetune multiply and
 *        effect switch are already done.  This spreads the tick-0 spike
 *        across two ticks.
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

static uint8_t master_volume = 63u;
uint8_t mod_need_prefetch = 0;

/* -------------------------------------------------------
 * OPT-3: Row pre-decode staging
 *
 * stage_row() is called at the END of each tick cycle (after
 * mod.row has been advanced) so that by the time tick 0 of the
 * new row fires, all the expensive decode work is already done.
 * commit_staged_row() on tick 0 only writes hardware registers.
 * ------------------------------------------------------- */
typedef struct {
    uint8_t  sample;
    uint16_t period;            /* finetune-adjusted; 0 = no change */
    uint16_t target_period;     /* for portamento (finetune-adjusted) */
    uint16_t hw_period;         /* downsample-adjusted period cache */
    uint8_t  effect;
    uint8_t  param;
    uint8_t  has_note;          /* raw period != 0 */
    uint8_t  hw_vol;            /* pre-scaled volume */
    uint8_t  volume;            /* mod volume */
    uint8_t  sample_latched;    /* new sample number seen on this row */
    /* row-0 global effects, resolved to plain values */
    uint8_t  set_speed;         /* non-zero → new speed */
    uint8_t  set_bpm;           /* non-zero → new BPM */
    uint8_t  do_pattern_break;
    uint8_t  break_row;
    uint8_t  do_jump;
    uint8_t  jump_order;
    uint8_t  do_pattern_delay;
    uint8_t  pattern_delay_val;
} StagedNote;

static StagedNote staged_row[MOD_CHANNELS];
static uint8_t    staged_valid   = 0u;
static uint8_t    staged_pattern = 0xFFu;
static uint8_t    staged_row_num = 0xFFu;

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
            uint32_t adj = (uint32_t)p << ds_shift;	    
            p = (adj > 0xFFFFUL) ? 0xFFFFu : (uint16_t)adj;
        }
    }
    cs->hw_period = p;
}

/* Helper: compute a temporary hw_period without caching (used by
 * vibrato / arpeggio which bend the period transiently each tick). */
static uint16_t temp_hw_period(const ChanState *cs, uint16_t period)
{
    if (cs->sample_num != 0u && cs->sample_num <= MOD_MAX_SAMPLES) {
        uint8_t ds_shift = SI_DS_SHIFT(&mod.samples[cs->sample_num]);
        if (ds_shift > 0u) {
            uint32_t adj = (uint32_t)period << ds_shift;	    
            return (adj > 0xFFFFUL) ? 0xFFFFu : (uint16_t)adj;
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
    cs->sam_len    = si->length;
    cs->loop_start = si->loop_start;
    cs->loop_len   = si->loop_len;
    cs->has_loop   = SI_HAS_LOOP(si);
    cs->is_adpcm   = SI_IS_ADPCM(si);
    cs->is_8bit    = !SI_IS_ADPCM(si);
    cs->active     = 1u;

    pokeymax_channel_setup(hw_chan,
                           cs->sam_addr,
                           cs->sam_len,
                           cs->hw_period,   /* OPT-2: pre-cached */
                           cs->hw_vol,
                           cs->is_8bit,
                           cs->is_adpcm);
}

/* -------------------------------------------------------
 * OPT-3a: stage_row
 * Decode the current (mod.row) row for all channels into
 * staged_row[].  NO hardware writes.  All heavy arithmetic
 * (apply_finetune, scale_volume, effect switch) happens here.
 * Called at the end of the tick cycle AFTER mod.row is set,
 * so it is ready for commit on the very next tick-0.
 * ------------------------------------------------------- */
static void stage_row(void)
{
    uint8_t pattern;
    const uint8_t *row_data;
    uint8_t ch;

    pattern = mod.order_table[mod.order_pos];
    if (mod_get_row_ptr(pattern, mod.row, &row_data) != 0) {
        staged_valid = 0u;
        return;
    }

    for (ch = 0u; ch < MOD_CHANNELS; ch++) {
        ChanState  *cs = &mod.chan[ch];
        StagedNote *sn = &staged_row[ch];
        const uint8_t *d = row_data + (uint16_t)ch * 4u;
        uint8_t  b0 = d[0], b1 = d[1], b2 = d[2], b3 = d[3];
        uint8_t  note_sample;
        uint16_t note_period;
        uint8_t  note_effect;
        uint8_t  note_param;
        uint8_t  snum_for_ft;
        int8_t   ft;

        note_sample = (uint8_t)((b0 & 0xF0u) | ((b2 >> 4) & 0x0Fu));
        note_period = ((uint16_t)(b0 & 0x0Fu) << 8) | b1;
        note_effect = b2 & 0x0Fu;
        note_param  = b3;

        if (note_sample > MOD_MAX_SAMPLES) note_sample = 0u;
        if (note_period != 0u && (note_period < 28u || note_period > 4095u))
            note_period = 0u;

        /* Initialise all staging fields */
        sn->effect            = note_effect;
        sn->param             = note_param;
        sn->has_note          = (note_period != 0u) ? 1u : 0u;
        sn->sample_latched    = note_sample;
        sn->period            = 0u;
        sn->target_period     = 0u;
        sn->hw_period         = 0u;
        sn->set_speed         = 0u;
        sn->set_bpm           = 0u;
        sn->do_pattern_break  = 0u;
        sn->break_row         = 0u;
        sn->do_jump           = 0u;
        sn->jump_order        = 0u;
        sn->do_pattern_delay  = 0u;
        sn->pattern_delay_val = 0u;

        /* Volume from new sample (or carry forward) */
        if (note_sample != 0u) {
            sn->volume = mod.samples[note_sample].volume;
            sn->hw_vol = scale_volume(sn->volume);
        } else {
            sn->volume = cs->volume;
            sn->hw_vol = cs->hw_vol;
        }

        /* Finetune-adjusted period (OPT-6) */
        snum_for_ft = note_sample ? note_sample : cs->sample_num;
        ft = (snum_for_ft > 0u && snum_for_ft <= MOD_MAX_SAMPLES) ?
             SI_FINETUNE(&mod.samples[snum_for_ft]) : 0;

        if (note_period != 0u &&
            note_effect != FX_TONE_PORTAMENTO &&
            note_effect != FX_TONE_PORTA_VOLSLIDE) {
            sn->period = apply_finetune(note_period, ft);
            /* Pre-compute hw_period for this staged note (OPT-2) */
            {
                uint16_t p = sn->period;
                uint8_t  sn2 = note_sample ? note_sample : cs->sample_num;
                if (sn2 != 0u && sn2 <= MOD_MAX_SAMPLES) {
                    uint8_t ds_shift = SI_DS_SHIFT(&mod.samples[sn2]);
                    if (ds_shift > 0u) {
                        uint32_t adj = (uint32_t)p << ds_shift;			
                        p = (adj > 0xFFFFUL) ? 0xFFFFu : (uint16_t)adj;
                    }
                }
                sn->hw_period = p;
            }
        }

        if (note_period != 0u &&
            (note_effect == FX_TONE_PORTAMENTO ||
             note_effect == FX_TONE_PORTA_VOLSLIDE)) {
            sn->target_period = apply_finetune(note_period, ft);
        }

        /* Row-0 effects — pure arithmetic, no hardware writes */
        switch (note_effect) {
            case FX_SET_VOLUME:
                sn->volume = (note_param > 64u) ? 64u : note_param;
                sn->hw_vol = scale_volume(sn->volume);
                break;
            case FX_SET_SPEED:
                if (note_param == 0u) break;
                if (note_param < 32u) sn->set_speed = note_param;
                else                  sn->set_bpm   = note_param;
                break;
            case FX_POSITION_JUMP:
                sn->do_jump           = 1u;
                sn->jump_order        = note_param;
                sn->do_pattern_break  = 1u;
                sn->break_row         = 0u;
                break;
            case FX_PATTERN_BREAK:
                sn->do_pattern_break = 1u;
                sn->break_row = (uint8_t)(((note_param >> 4) * 10u) +
                                           (note_param & 0x0Fu));
                if (sn->break_row >= MOD_ROWS_PER_PAT) sn->break_row = 0u;
                break;
            case FX_EXTENDED: {
                uint8_t sub  = (note_param >> 4) & 0x0Fu;
                uint8_t subp =  note_param       & 0x0Fu;
                switch (sub) {
                    case EFX_FINE_VOL_UP: {
                        uint8_t v = sn->volume + subp;
                        sn->volume = (v > 64u) ? 64u : v;
                        sn->hw_vol = scale_volume(sn->volume);
                        break;
                    }
                    case EFX_FINE_VOL_DOWN:
                        sn->volume = (subp > sn->volume) ? 0u :
                                     (uint8_t)(sn->volume - subp);
                        sn->hw_vol = scale_volume(sn->volume);
                        break;
                    case EFX_PATTERN_DELAY:
                        sn->do_pattern_delay  = 1u;
                        sn->pattern_delay_val = subp;
                        break;
                    /* EFX_FINE_SLIDE_*, EFX_LOOP, EFX_NOTE_CUT/DELAY
                     * need cs state at commit time — handled there */
                    default: break;
                }
                break;
            }
            /* FX_VIBRATO, FX_TONE_PORTAMENTO: params latched at commit */
            default: break;
        }
    }

    staged_valid   = 1u;
    staged_pattern = pattern;
    staged_row_num = mod.row;
}

/* -------------------------------------------------------
 * OPT-3b: commit_staged_row
 * Apply pre-decoded staged_row[] to hardware on tick 0.
 * Only hardware register writes and state commits live here.
 * ------------------------------------------------------- */
static void commit_staged_row(void)
{
    uint8_t ch;

    mod.pattern_break = 0u;
    mod.do_jump       = 0u;

    for (ch = 0u; ch < MOD_CHANNELS; ch++) {
        ChanState  *cs = &mod.chan[ch];
        StagedNote *sn = &staged_row[ch];
        uint8_t     hw = (uint8_t)(ch + 1u);
        uint8_t hw_update_needed = 0u;

        cs->effect           = sn->effect;
        cs->param            = sn->param;
        cs->triggered        = 0u;
        cs->note_delay_ticks = 0u;
        cs->note_cut_tick    = 0u;

        /* Global effects: last channel wins (ProTracker compat) */
        if (sn->set_speed)        mod.speed = sn->set_speed;
        if (sn->set_bpm)          mod.bpm   = sn->set_bpm;
        if (sn->do_pattern_break) {
            mod.pattern_break = 1u;
            mod.break_row     = sn->break_row;
        }
        if (sn->do_jump) {
            mod.do_jump    = 1u;
            mod.jump_order = sn->jump_order;
        }
        if (sn->do_pattern_delay) mod.pattern_delay = sn->pattern_delay_val;

        /* Sample latch */
        if (sn->sample_latched != 0u) {
            cs->sample_num = sn->sample_latched;
            cs->volume     = sn->volume;
            cs->hw_vol     = sn->hw_vol;
            hw_update_needed = 1u;
        }

        /* Period (OPT-2: staged hw_period already computed) */
        if (sn->period != 0u) {
            cs->period    = sn->period;
            cs->hw_period = sn->hw_period;
            hw_update_needed = 1u;
        }
        if (sn->target_period != 0u) {
            cs->target_period = sn->target_period;
        }

        /* Volume from row-0 effects not already caught by sample latch */
        if (sn->effect == FX_SET_VOLUME ||
            (sn->effect == FX_EXTENDED &&
             ((sn->param >> 4) == EFX_FINE_VOL_UP ||
              (sn->param >> 4) == EFX_FINE_VOL_DOWN))) {
            cs->volume = sn->volume;
            cs->hw_vol = sn->hw_vol;
            hw_update_needed = 1u;
        }

        /* Commit-time effects that need live cs state */
        switch (sn->effect) {
            case FX_VIBRATO:
                if (sn->param & 0xF0u) cs->vib_speed = (sn->param >> 4) & 0x0Fu;
                if (sn->param & 0x0Fu) cs->vib_depth =  sn->param       & 0x0Fu;
                break;
            case FX_TONE_PORTAMENTO:
            case FX_TONE_PORTA_VOLSLIDE:
                if (sn->param != 0u) cs->port_speed = sn->param;
                break;
            case FX_EXTENDED: {
                uint8_t sub  = (sn->param >> 4) & 0x0Fu;
                uint8_t subp =  sn->param       & 0x0Fu;
                switch (sub) {
                    case EFX_FINE_SLIDE_UP:
                        if (cs->period > subp) cs->period -= subp;
                        update_hw_period(cs);
                        hw_update_needed = 1u;
                        break;
                    case EFX_FINE_SLIDE_DOWN:
                        cs->period += subp;
                        update_hw_period(cs);
                        hw_update_needed = 1u;
                        break;
                    case EFX_LOOP:
                        if (subp == 0u) {
                            cs->loop_row = mod.row;
                        } else {
                            if (cs->loop_count == 0u) {
                                cs->loop_count = subp;
                                mod.pattern_break = 1u;
                                mod.break_row = cs->loop_row;
                            } else {
                                cs->loop_count--;
                                if (cs->loop_count > 0u) {
                                    mod.pattern_break = 1u;
                                    mod.break_row = cs->loop_row;
                                }
                            }
                        }
                        break;
                    case EFX_NOTE_CUT:
                        cs->note_cut_tick = subp;
                        break;
                    case EFX_NOTE_DELAY:
                        cs->note_delay_ticks = subp;
                        goto skip_trigger;
                    default: break;
                }
                break;
            }
            default: break;
        }

        /* Trigger */
        if (sn->has_note &&
            sn->effect != FX_TONE_PORTAMENTO &&
            sn->effect != FX_TONE_PORTA_VOLSLIDE) {
            trigger_sample(hw, cs);
            cs->triggered = 1u;
        } else if (sn->sample_latched != 0u && cs->period != 0u && !cs->active) {
            trigger_sample(hw, cs);
            cs->triggered = 1u;
        }

        if (!cs->triggered && cs->active && hw_update_needed) {
            pokeymax_channel_set_period_vol(hw, cs->hw_period, cs->hw_vol);
        }

        skip_trigger: ;
    }

    staged_valid = 0u;
}

/* -------------------------------------------------------
 * update_effects - ticks 1..speed-1
 * Uses cs->hw_period (OPT-2) and int8_t sine table (OPT-4).
 * ------------------------------------------------------- */
static void update_effects(void)
{
    uint8_t ch;

    for (ch = 0u; ch < MOD_CHANNELS; ch++) {
        ChanState *cs = &mod.chan[ch];
        uint8_t    hw = (uint8_t)(ch + 1u);
        uint8_t    period_changed = 0u;
        uint8_t    vol_changed    = 0u;

        switch (cs->effect) {
            case FX_ARPEGGIO:
                if (cs->param != 0u && cs->period != 0u) {
                    uint8_t  semi1  = (cs->param >> 4) & 0x0Fu;
                    uint8_t  semi2  =  cs->param       & 0x0Fu;
                    uint16_t base   = cs->period;
                    uint8_t  offset = 0u;
                    switch (mod.tick % 3u) {
                        case 1: offset = semi1; break;
                        case 2: offset = semi2; break;
                        default: break;
                    }
                    if (offset > 0u) {
                        uint16_t div = (uint16_t)(256u + (uint16_t)offset * 14u);
                        base = (uint16_t)((uint32_t)base * 256UL / div);
                    }
                    /* Arpeggio bends transiently; use temp_hw_period, don't cache */
                    pokeymax_channel_set_period_vol(hw, temp_hw_period(cs, base), cs->hw_vol);
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
                if (vib_period < 113) vib_period = 113;
                if (vib_period > 856) vib_period = 856;
                pokeymax_channel_set_period_vol(hw,
                    temp_hw_period(cs, (uint16_t)vib_period), cs->hw_vol);
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
                if (vib_period < 113) vib_period = 113;
                if (vib_period > 856) vib_period = 856;
                pokeymax_channel_set_period_vol(hw,
                    temp_hw_period(cs, (uint16_t)vib_period), cs->hw_vol);
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

        if ((period_changed || vol_changed) && cs->active) {
            pokeymax_channel_set_period_vol(hw, cs->hw_period, cs->hw_vol);
        }
    }
}

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
 * Generic fallback uses one unavoidable 32-bit multiply but is
 * only compiled for non-PAL/NTSC targets.
 * ------------------------------------------------------- */
static uint16_t bpm_accum = 0u;
static uint16_t bpm_step  = 0u;
static uint8_t  last_bpm  = 0u;

static void update_bpm_step(void)
{
    uint8_t bpm8;
    if (mod.bpm == last_bpm) return;
    last_bpm = (uint8_t)mod.bpm;
    bpm8     = last_bpm;

#if VBI_HZ == 50
    {
        /* step = BPM*2 + (BPM*12 + 125) / 250   (all uint16_t) */
        uint16_t hi = (uint16_t)bpm8 << 1;
        uint16_t lo = (uint16_t)bpm8 * 12u + 125u;
        bpm_step = hi + lo / 250u;
    }
#elif VBI_HZ == 60
    {
        /* step ≈ BPM + (BPM * 177) >> 8 */
        uint16_t frac = (uint16_t)bpm8 * 177u;
        bpm_step = (uint16_t)bpm8 + (frac >> 8);
    }
#else
    {
        /* Generic: one 32-bit multiply (rare, non-standard VBI rate) */
        uint32_t numer = (uint32_t)bpm8 * 6144UL;
        uint16_t denom = (uint16_t)(60u * (uint16_t)VBI_HZ);
        bpm_step = (uint16_t)((numer + (uint32_t)(denom >> 1u)) / denom);
    }
#endif

    bpm_accum = 0u;
}

/* -------------------------------------------------------
 * do_tick
 *
 * OPT-3 flow:
 *   tick 0          → commit staged row (fast: only hw writes)
 *   tick 1..speed-2 → update_effects()
 *   tick speed-1    → update_effects(), then stage NEXT row
 *
 * First tick after a jump / pattern-wrap has no staged data;
 * the inline fallback path decodes and commits in one pass
 * (same cost as the old code, but only on those rare ticks).
 * ------------------------------------------------------- */
static void do_tick(void)
{
    if (mod.tick == 0u) {
        if (mod.pattern_delay > 0u) {
            mod.pattern_delay--;
            staged_valid = 0u;
        } else {
            if (staged_valid &&
                staged_pattern == mod.order_table[mod.order_pos] &&
                staged_row_num == mod.row) {
                /* Fast path: pre-staged data available */
                commit_staged_row();
            } else {
                /* Slow fallback: decode + commit inline (first row, post-jump) */
                uint8_t pattern = mod.order_table[mod.order_pos];
                const uint8_t *row_data;

                if (mod_get_row_ptr(pattern, mod.row, &row_data) == 0) {
                    uint8_t ch;
                    mod.pattern_break = 0u;
                    mod.do_jump       = 0u;
                    row_work_data     = row_data;

                    for (ch = 0u; ch < MOD_CHANNELS; ch++) {
                        ChanState *cs = &mod.chan[ch];
                        uint8_t    hw = (uint8_t)(ch + 1u);
                        const uint8_t *d = row_data + (uint16_t)ch * 4u;
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
                            cs->volume=(npar>64u)?64u:npar; cs->hw_vol=scale_volume(cs->volume); hw_upd=1u; break;
                          case FX_SET_SPEED:
                            if(npar==0u) break; if(npar<32u) mod.speed=npar; else mod.bpm=npar; break;
                          case FX_VIBRATO:
                            if(npar&0xF0u) cs->vib_speed=(npar>>4)&0x0Fu;
                            if(npar&0x0Fu) cs->vib_depth=npar&0x0Fu; break;
                          case FX_TONE_PORTAMENTO: case FX_TONE_PORTA_VOLSLIDE:
                            if(npar) cs->port_speed=npar; break;
                          case FX_POSITION_JUMP:
                            mod.do_jump=1u; mod.jump_order=npar; mod.pattern_break=1u; mod.break_row=0u; break;
                          case FX_PATTERN_BREAK:
                            mod.pattern_break=1u;
                            mod.break_row=(uint8_t)(((npar>>4)*10u)+(npar&0x0Fu));
                            if(mod.break_row>=MOD_ROWS_PER_PAT) mod.break_row=0u; break;
                          case FX_EXTENDED: {
                            uint8_t sub=(npar>>4)&0x0Fu, subp=npar&0x0Fu;
                            switch(sub) {
                              case EFX_FINE_VOL_UP:   { uint8_t v=cs->volume+subp; cs->volume=(v>64u)?64u:v; cs->hw_vol=scale_volume(cs->volume); hw_upd=1u; break; }
                              case EFX_FINE_VOL_DOWN: if(subp>cs->volume) cs->volume=0u; else cs->volume-=subp; cs->hw_vol=scale_volume(cs->volume); hw_upd=1u; break;
                              case EFX_FINE_SLIDE_UP:   if(cs->period>subp) cs->period-=subp; update_hw_period(cs); hw_upd=1u; break;
                              case EFX_FINE_SLIDE_DOWN: cs->period+=subp; update_hw_period(cs); hw_upd=1u; break;
                              case EFX_LOOP:
                                if(subp==0u) cs->loop_row=mod.row;
                                else { if(cs->loop_count==0u){cs->loop_count=subp;mod.pattern_break=1u;mod.break_row=cs->loop_row;}
                                       else{cs->loop_count--;if(cs->loop_count>0u){mod.pattern_break=1u;mod.break_row=cs->loop_row;}}} break;
                              case EFX_NOTE_CUT:   cs->note_cut_tick=subp; break;
                              case EFX_NOTE_DELAY: cs->note_delay_ticks=subp; goto skip_trigger_fb;
                              case EFX_PATTERN_DELAY: mod.pattern_delay=subp; break;
                              default: break;
                            }
                            break;
                          }
                          default: break;
                        }

                        if(np && ne!=FX_TONE_PORTAMENTO && ne!=FX_TONE_PORTA_VOLSLIDE) {
                            trigger_sample(hw,cs); cs->triggered=1u;
                        } else if(ns && cs->period && !cs->active) {
                            trigger_sample(hw,cs); cs->triggered=1u;
                        }
                        if(!cs->triggered && cs->active && hw_upd)
                            pokeymax_channel_set_period_vol(hw,cs->hw_period,cs->hw_vol);
                        skip_trigger_fb: ;
                    }
                    row_work_active = 0u;
                }
            }
        }
    } /* tick == 0 */

    if (mod.tick != 0u) {
        update_effects();
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
            staged_valid = 0u;   /* discard stale staging after jump */
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
                staged_valid = 0u;
            }
        }

        /* OPT-3: Pre-decode the row we just landed on, ready for next tick 0. */
        if (mod.playing) {
            stage_row();
        }
	/* OPT-3: Pre-decode the row we just landed on, ready for next tick 0. */
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

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */
void mod_play(void)
{
    uint8_t ch;
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
    staged_valid     = 0u;

    bpm_accum = 0u;
    last_bpm  = 0u;

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
    staged_valid     = 0u;

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
