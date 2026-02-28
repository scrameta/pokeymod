/*
 * modplayer.c - MOD player engine
 *
 * Key fixes vs previous version:
 *
 * 1. DMA is now enabled per-channel at trigger time, not upfront.
 *    pokeymax_enable_all() is gone; pokeymax_channel_dma_on() is used instead.
 *
 * 2. trigger_sample() now always triggers on a new note (period != 0),
 *    regardless of cs->active.  Previous logic blocked retrigger when
 *    active==1, causing silence after the first sample played out.
 *
 * 3. All hardware register writes go through PEEK/POKE (via pokeymax_hw.c).
 *    No volatile macros remain in this file.
 *
 * 4. mod_play() no longer calls pokeymax_enable_all().  It only enables
 *    IRQs for all channels (so loop handler fires), then the first real
 *    note triggers DMA.
 */

#include <stdint.h>
#include <string.h>
#include "mod_format.h"
#include "modplayer.h"
#include "pokeymax.h"
#include "pokeymax_hw.h"

ModPlayer mod;

extern const uint16_t amiga_periods[NUM_PERIODS];
extern const uint16_t finetune_ratio[16];
extern const uint8_t  vibrato_sine[64];

static uint8_t master_volume = 63;

/* -------------------------------------------------------
 * Period conversion: hw = 2 * amiga_period
 * (PokeyMAX core clock = 2 * Paula clock)
 * ------------------------------------------------------- */
static uint16_t period_to_hw(uint16_t p)
{
    uint32_t hw = (uint32_t)p * 2UL;  /* PokeyMAX clock=2*PHI2, same ratio as Paula */
    if (hw == 0UL)       return 1;
    if (hw > 0xFFFFUL)   return 0xFFFF;
    return (uint16_t)hw;
}

static uint16_t apply_finetune(uint16_t period, int8_t ft)
{
    uint32_t adj;
    uint8_t  idx;
    if (ft == 0) return period;
    idx = (uint8_t)(ft + 8);
    adj = ((uint32_t)period * finetune_ratio[idx]) >> 8;
    if (adj == 0) adj = 1;
    return (uint16_t)adj;
}

static uint8_t scale_volume(uint8_t mod_vol)
{
    uint16_t v = (uint16_t)mod_vol * (uint16_t)master_volume;
    return (uint8_t)(v / 64u);
}

/* -------------------------------------------------------
 * mod_decode_note
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
 * Trigger a sample on a hardware channel.
 * Always sets up the channel fully and enables DMA.
 * ------------------------------------------------------- */
static void trigger_sample(uint8_t hw_chan, ChanState *cs)
{
    SampleInfo *si;
    uint16_t    hw_period;

    if (cs->sample_num == 0 || cs->sample_num > MOD_MAX_SAMPLES) return;
    si = &mod.samples[cs->sample_num];
    if (si->length == 0) return;
    if (si->pokeymax_len == 0) return;
    if (cs->period == 0) return;

    hw_period = period_to_hw(cs->period);

    cs->sam_addr   = si->pokeymax_addr;
    cs->sam_len    = si->length;
    cs->loop_start = si->loop_start;
    cs->loop_len   = si->loop_len;
    cs->has_loop   = si->has_loop;
    cs->is_adpcm   = si->is_adpcm;
    cs->is_8bit    = si->is_8bit;
    cs->active     = 1;

    pokeymax_channel_setup(hw_chan,
                           cs->sam_addr,
                           cs->sam_len,
                           hw_period,
                           cs->hw_vol,
                           cs->is_8bit,
                           cs->is_adpcm);

    /* Enable DMA for this channel now that it is configured */
    pokeymax_channel_dma_on(hw_chan);
}

/* -------------------------------------------------------
 * process_row - called on tick 0
 * ------------------------------------------------------- */
static void process_row(void)
{
    uint8_t ch;
    uint8_t pattern;

    pattern = mod.order_table[mod.order_pos];
    if (mod_read_row(pattern, mod.row) != 0) return;

    mod.pattern_break = 0;
    mod.do_jump       = 0;

    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        Note       note;
        ChanState *cs = &mod.chan[ch];
        uint8_t    hw = (uint8_t)(ch + 1u);
        MODNote    raw;

        raw.raw[0] = mod_row_buf[ch * 4u + 0u];
        raw.raw[1] = mod_row_buf[ch * 4u + 1u];
        raw.raw[2] = mod_row_buf[ch * 4u + 2u];
        raw.raw[3] = mod_row_buf[ch * 4u + 3u];

        mod_decode_note(&raw, &note);

        cs->effect            = note.effect;
        cs->param             = note.param;
        cs->triggered         = 0;
        cs->note_delay_ticks  = 0;
        cs->note_cut_tick     = 0;

        /* New sample number: latch volume (guard invalid sample numbers) */
        if (note.sample > MOD_MAX_SAMPLES) {
            note.sample = 0u;
        }
        if (note.sample != 0u) {
            cs->sample_num = note.sample;
            cs->volume     = mod.samples[note.sample].volume;
            cs->hw_vol     = scale_volume(cs->volume);
        }

        /* Defensive: invalid latched sample from corrupt pattern data */
        if (cs->sample_num > MOD_MAX_SAMPLES) {
            cs->sample_num = 0u;
        }

        /* New period (unless portamento) */
        if (note.period != 0 && note.effect != FX_TONE_PORTAMENTO) {
            int8_t ft = (cs->sample_num > 0) ?
                        mod.samples[cs->sample_num].finetune : 0;
            cs->period = apply_finetune(note.period, ft);
        }
        if (note.period != 0 && note.effect == FX_TONE_PORTAMENTO) {
            int8_t ft = (cs->sample_num > 0) ?
                        mod.samples[cs->sample_num].finetune : 0;
            cs->target_period = apply_finetune(note.period, ft);
        }

        /* Row-0 effects */
        switch (note.effect) {
            case FX_SET_VOLUME:
                cs->volume = (note.param > 64u) ? 64u : note.param;
                cs->hw_vol = scale_volume(cs->volume);
                break;
            case FX_SET_SPEED:
                if (note.param == 0u) break;
                if (note.param < 32u) mod.speed = note.param;
                else                  mod.bpm   = note.param;
                break;
            case FX_VIBRATO:
                if (note.param & 0xF0u) cs->vib_speed = (note.param >> 4) & 0x0Fu;
                if (note.param & 0x0Fu) cs->vib_depth =  note.param       & 0x0Fu;
                break;
            case FX_TONE_PORTAMENTO:
                if (note.param != 0u) cs->port_speed = note.param;
                break;
            case FX_PATTERN_BREAK:
                mod.pattern_break = 1;
                mod.break_row = (uint8_t)(((note.param >> 4) * 10u) + (note.param & 0x0Fu));
                if (mod.break_row >= MOD_ROWS_PER_PAT) mod.break_row = 0;
                break;
            case FX_EXTENDED: {
                uint8_t sub  = (note.param >> 4) & 0x0Fu;
                uint8_t subp =  note.param       & 0x0Fu;
                switch (sub) {
                    case EFX_FINE_VOL_UP:
                        cs->volume += subp;
                        if (cs->volume > 64u) cs->volume = 64u;
                        cs->hw_vol = scale_volume(cs->volume);
                        break;
                    case EFX_FINE_VOL_DOWN:
                        if (subp > cs->volume) cs->volume = 0;
                        else cs->volume -= subp;
                        cs->hw_vol = scale_volume(cs->volume);
                        break;
                    case EFX_FINE_SLIDE_UP:
                        if (cs->period > subp) cs->period -= subp;
                        break;
                    case EFX_FINE_SLIDE_DOWN:
                        cs->period += subp;
                        break;
                    case EFX_LOOP:
                        if (subp == 0u) {
                            cs->loop_row = mod.row;
                        } else {
                            if (cs->loop_count == 0u) {
                                cs->loop_count = subp;
                                mod.pattern_break = 1;
                                mod.break_row = cs->loop_row;
                            } else {
                                cs->loop_count--;
                                if (cs->loop_count > 0u) {
                                    mod.pattern_break = 1;
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
                        goto skip_trigger;   /* delay the note */
                    case EFX_PATTERN_DELAY:
                        mod.pattern_delay = subp;
                        break;
                    default: break;
                }
                break;
            }
            default: break;
        }

        /* MOD periods are normally in ~113..856 range; ignore junk */
        if (note.period != 0u && (note.period < 28u || note.period > 4095u)) {
            note.period = 0u;
        }

        /* Trigger sample if we have a new note (period != 0) */
        if (note.period != 0u && note.effect != FX_TONE_PORTAMENTO) {
            trigger_sample(hw, cs);
            cs->triggered = 1;
        } else if (note.sample != 0u && cs->period != 0u && !cs->active) {
            /* Sample number given without period on a previously silent channel */
            trigger_sample(hw, cs);
            cs->triggered = 1;
        }

        /* If no trigger happened, push any volume change to hardware */
        if (!cs->triggered && cs->active) {
            pokeymax_channel_set_period_vol(hw, period_to_hw(cs->period), cs->hw_vol);
        }

        skip_trigger: ;
    }
}

/* -------------------------------------------------------
 * update_effects - called on ticks 1..speed-1
 * ------------------------------------------------------- */
static void update_effects(void)
{
    uint8_t ch;

    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        ChanState *cs = &mod.chan[ch];
        uint8_t    hw = (uint8_t)(ch + 1u);
        uint8_t    period_changed = 0;
        uint8_t    vol_changed    = 0;

        switch (cs->effect) {
            case FX_ARPEGGIO:
                if (cs->param != 0u && cs->period != 0u) {
                    uint8_t  semi1 = (cs->param >> 4) & 0x0Fu;
                    uint8_t  semi2 =  cs->param       & 0x0Fu;
                    uint16_t base  = cs->period;
                    uint8_t  offset = 0;
                    switch (mod.tick % 3u) {
                        case 1: offset = semi1; break;
                        case 2: offset = semi2; break;
                        default: break;
                    }
                    if (offset > 0u) {
                        uint16_t div = (uint16_t)(256u + (uint16_t)offset * 14u);
                        base = (uint16_t)((uint32_t)base * 256UL / div);
                    }
                    pokeymax_channel_set_period_vol(hw, period_to_hw(base), cs->hw_vol);
                }
                break;

            case FX_SLIDE_UP:
                if (cs->period > cs->param) cs->period -= cs->param;
                else cs->period = 113u;
                if (cs->period < 113u) cs->period = 113u;
                period_changed = 1;
                break;

            case FX_SLIDE_DOWN:
                cs->period += cs->param;
                if (cs->period > 856u) cs->period = 856u;
                period_changed = 1;
                break;

            case FX_TONE_PORTAMENTO:
                if (cs->period < cs->target_period) {
                    cs->period += cs->port_speed;
                    if (cs->period > cs->target_period) cs->period = cs->target_period;
                } else if (cs->period > cs->target_period) {
                    if (cs->period >= cs->port_speed) cs->period -= cs->port_speed;
                    if (cs->period < cs->target_period)  cs->period = cs->target_period;
                }
                period_changed = 1;
                break;

            case FX_VIBRATO: {
                uint8_t  vib_idx = cs->vib_pos & 63u;
                int16_t  sv      = (int16_t)vibrato_sine[vib_idx];
                int16_t  delta;
                int16_t  vib_period;
                if (sv > 127) sv -= 256;
                delta = (int16_t)((sv * (int16_t)cs->vib_depth) >> 7);
                vib_period = (int16_t)cs->period + delta;
                if (vib_period < 113) vib_period = 113;
                if (vib_period > 856) vib_period = 856;
                pokeymax_channel_set_period_vol(hw, period_to_hw((uint16_t)vib_period), cs->hw_vol);
                cs->vib_pos = (uint8_t)((cs->vib_pos + cs->vib_speed) & 63u);
                break;
            }

            case FX_EXTENDED: {
                uint8_t sub  = (cs->param >> 4) & 0x0Fu;
                uint8_t subp =  cs->param       & 0x0Fu;
                if (sub == EFX_NOTE_CUT && mod.tick == cs->note_cut_tick) {
                    cs->volume = 0; cs->hw_vol = 0; vol_changed = 1;
                }
                if (sub == EFX_NOTE_DELAY && mod.tick == cs->note_delay_ticks) {
                    trigger_sample(hw, cs); cs->triggered = 1;
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
            pokeymax_channel_set_period_vol(hw, period_to_hw(cs->period), cs->hw_vol);
        }
    }
}

/* -------------------------------------------------------
 * BPM timing (8.8 fixed-point accumulator)
 * step = (VBI_HZ * 640) / BPM
 * PAL 50Hz, BPM 125 → step = 256 = exactly 1 tick per VBI
 * ------------------------------------------------------- */
static uint16_t bpm_accum = 0;
static uint16_t bpm_step  = 0;
static uint16_t last_bpm  = 0;

static void update_bpm_step(void)
{
    if (mod.bpm == last_bpm) return;
    last_bpm  = mod.bpm;
    bpm_step  = (uint16_t)(((uint32_t)VBI_HZ * 640UL) / (uint32_t)mod.bpm);
    bpm_accum = 0;
}

/* -------------------------------------------------------
 * do_tick - advance one tick
 * ------------------------------------------------------- */
static void do_tick(void)
{
    if (mod.tick == 0u) {
        if (mod.pattern_delay > 0u) {
            mod.pattern_delay--;
        } else {
            process_row();
        }
    } else {
        update_effects();
    }

    mod.tick++;

    if (mod.tick >= mod.speed) {
        mod.tick = 0;

        if (mod.pattern_break) {
            mod.pattern_break = 0;
            mod.row = mod.break_row;
            mod.order_pos++;
            if (mod.order_pos >= mod.song_length) {
                if (mod.loop_song) mod.order_pos = 0;
                else { mod.playing = 0; return; }
            }
            /* Signal pattern buffer swap + prefetch */
            {
                uint8_t next_order = (uint8_t)(mod.order_pos + 1u);
                if (next_order >= mod.song_length) next_order = 0;
                mod_pattern_advance(mod.order_table[mod.order_pos],
                                    mod.order_table[next_order]);
            }
        } else {
            mod.row++;
            if (mod.row >= MOD_ROWS_PER_PAT) {
                mod.row = 0;
                mod.order_pos++;
                if (mod.order_pos >= mod.song_length) {
                    if (mod.loop_song) mod.order_pos = 0;
                    else { mod.playing = 0; return; }
                }
                /* Signal pattern buffer swap + prefetch */
                {
                    uint8_t next_order = (uint8_t)(mod.order_pos + 1u);
                    if (next_order >= mod.song_length) next_order = 0;
                    mod_pattern_advance(mod.order_table[mod.order_pos],
                                        mod.order_table[next_order]);
                }
            }
        }
    }
}

/* -------------------------------------------------------
 * mod_vbi_tick - called from deferred VBI ISR
 * ------------------------------------------------------- */
void mod_vbi_tick(void)
{
    if (!mod.playing) return;
    update_bpm_step();
    bpm_accum += bpm_step;
    while (bpm_accum >= 256u) {
        bpm_accum -= 256u;
        do_tick();
        if (!mod.playing) return;
    }
}

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */
void mod_play(void)
{
    uint8_t ch;
    if (mod.playing) return;

    /* Reset all channel state */
    for (ch = 0; ch < MOD_CHANNELS; ch++) {
        mod.chan[ch].active    = 0;
        mod.chan[ch].sample_num= 0;
        mod.chan[ch].period    = 0;
        mod.chan[ch].volume    = 0;
        mod.chan[ch].hw_vol    = 0;
        mod.chan[ch].vib_pos   = 0;
    }

    mod.playing   = 1;
    mod.order_pos = 0;
    mod.row       = 0;
    mod.tick      = 0;
    bpm_accum     = 0;
    last_bpm      = 0;

    /* Keep sample IRQs disabled at startup. They are enabled per-channel in
     * pokeymax_channel_setup() when a real note is triggered on tick 0.
     * Enabling all channels here can generate immediate IRQs on unconfigured
     * channels and re-enter C while mod_play() is still running.
     */
    POKE(REG_DMA,    0x00);
    POKE(REG_IRQACT, 0x00);
    POKE(REG_IRQEN,  0x00);
}

void mod_stop(void)
{
    mod.playing = 0;
    POKE(REG_DMA,    0x00);
    POKE(REG_IRQEN,  0x00);
    POKE(REG_COVOX_CH1, 0);
    POKE(REG_COVOX_CH2, 0);
    POKE(REG_COVOX_CH3, 0);
    POKE(REG_COVOX_CH4, 0);
}

void mod_pause(void)
{
    mod.playing = (uint8_t)!mod.playing;
    if (!mod.playing) {
        POKE(REG_COVOX_CH1, 0);
        POKE(REG_COVOX_CH2, 0);
        POKE(REG_COVOX_CH3, 0);
        POKE(REG_COVOX_CH4, 0);
    }
}

void mod_set_volume(uint8_t vol) { master_volume = vol & 0x3Fu; }
uint8_t mod_get_row(void)        { return mod.row; }
uint8_t mod_get_order(void)      { return mod.order_pos; }
