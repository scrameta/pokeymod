/*
 * mod_loader.c - MOD loader with double-buffered pattern cache
 *
 */

#include <stdio.h>
#include <string.h>
#include "mod_format.h"
#include "mod_loader.h"
#include "mod_struct.h"
#include "pokeymax.h"
#include "pokeymax_hw.h"
#include "adpcm.h"

// TODO remove all printf

#define SECTOR_SIZE 256

static FILE    *mod_file            = NULL;

#pragma bss-name(push, "LOWBSS")
static uint8_t sector_buf[SECTOR_SIZE];
#pragma bss-name(pop)
static uint8_t adpcm_out[SECTOR_SIZE / 2];

uint8_t mod_row_buf[MOD_CHANNELS * 4];


static const ModLoadProgressPlugin *s_progress_plugin = 0;

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

void mod_file_close(void)
{
    if (mod_file) { fclose(mod_file); mod_file = NULL; }
}

void mod_set_load_progress_plugin(const ModLoadProgressPlugin *plugin)
{
    s_progress_plugin = plugin;
}


/* -------------------------------------------------------
 * mod_load()
 * ------------------------------------------------------- */
uint8_t mod_load(const char *filename)
{
    uint8_t  i;
    uint8_t  hdr_buf[30];
    uint8_t  title[20];
    uint8_t  two[2];
    uint8_t  magic[4];
    uint32_t total_sample_bytes;
    uint8_t  total_samples_to_load;
    uint8_t  loaded_samples;
    uint32_t loaded_source_bytes;
    uint32_t loaded_stored_bytes;
    uint8_t  progress_started;
    uint8_t  had_status_output;

    uint32_t sample_data_offset;

    memset(&mod, 0, sizeof(mod));

    strncpy(mod.filename, filename, 64);

    if (mod_file) { fclose(mod_file); mod_file = NULL; }
    mod_file = fopen(filename, "rb");
    if (!mod_file) return 1;

    if (fread(title, 1, 20, mod_file) != 20) return 1;
    (void)title;

    total_sample_bytes = 0;
    total_samples_to_load = 0;
    for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
        SampleInfo *si = &mod.samples[i];
        if (fread(hdr_buf, 1, 30, mod_file) != 30) return 1;
        si->length     = read_be16(hdr_buf + 22) * 2u;
        {
            uint8_t ft_raw = hdr_buf[24] & 0x0Fu;  /* 0..15, where 8..15 = -8..-1 */
            si->flags = (uint8_t)(ft_raw << 4);
        }
        si->volume     = (hdr_buf[25] > 64u) ? 64u : hdr_buf[25];
        si->loop_start = read_be16(hdr_buf + 26) * 2u;
        si->loop_len   = read_be16(hdr_buf + 28) * 2u;
        {
            uint8_t has_loop = (si->loop_len > 2u) ? 1u : 0u;
            if (has_loop && (si->loop_start + si->loop_len) > si->length)
                si->loop_len = si->length - si->loop_start;
            si->flags = (si->flags & 0xFCu) | (has_loop ? SI_STYPE_PCM_LOOP : SI_STYPE_PCM);
        }	
        if (si->length > 0u) {
            total_sample_bytes += si->length;
            total_samples_to_load++;
        }
    }

    if (fread(two, 1, 2, mod_file) != 2) return 1;
    mod.song_length = two[0];

    if (fread(mod.order_table, 1, MOD_MAX_PATTERNS, mod_file) != MOD_MAX_PATTERNS) return 1;
    if (fread(magic, 1, 4, mod_file) != 4) return 1;
    (void)magic;

    mod.num_patterns = 0;
    for (i = 0; i < mod.song_length; i++)
        if (mod.order_table[i] > mod.num_patterns)
            mod.num_patterns = mod.order_table[i];
    mod.num_patterns++;

    mod.pattern_data_offset = (uint32_t)ftell(mod_file);
    mod.pattern_data_size = (uint32_t)mod.num_patterns * (uint32_t)PAT_BYTES;

    /* ------------------------------------------------------------------
     * PASS 1: Plan compression and downsampling from header data only.
     *
     * Compute how much PokeyMAX RAM each sample will need, then decide
     * downsample factors up-front so the load loop never needs to retry.
     *
     * Priority order for downsampling (impact lowest first):
     *   1. Looped PCM samples (largest, can't use ADPCM, often pads/bass
     *      where quality loss is masked by the loop repetition).
     *   2. Large non-looped PCM samples (already ADPCM if eligible, so
     *      these are short ones or ones below the ADPCM threshold).
     * Within each tier, prefer larger samples first (more bytes saved).
     *
     * We never downsample ADPCM samples -- byte-skipping on ADPCM data
     * would produce gibberish.
     *
     * No disk I/O here: all decisions are from mod.samples[] headers.
     * ------------------------------------------------------------------ */
    {
        /* Step A: initialise per-sample plan fields assuming no compression,
         * then apply ADPCM to non-looped samples if RAM is exceeded.
         * ADPCM halves stored size and is preferred over downsampling as it
         * has lower quality impact.  Downsampling is applied in Step B only
         * if ADPCM alone is insufficient.
         * Downsampling is applied before encoding, so cost is:
         *   ADPCM: (length / downsample_factor + 1) / 2
         *   PCM:    length / downsample_factor
         * downsample_factor starts at 1 for all samples. */
        uint32_t needed = 0u;
        uint8_t  j;

        for (j = 1u; j <= MOD_MAX_SAMPLES; j++) {
            SampleInfo *si = &mod.samples[j];
	    /* Preserve header-derived finetune (bits 7:4) and sample type/loop (bits 1:0).
             * Pass 1 should only reset downsample planning (bits 3:2).
             */
            si->flags &= (uint8_t)~0x0Cu;
            if (si->length == 0u) continue;
            needed += (uint32_t)si->length;
        }

        /* Apply ADPCM to eligible samples if we exceed RAM */
        if (needed > (uint32_t)POKEYMAX_RAM_SIZE) {
            for (j = 1u; j <= MOD_MAX_SAMPLES; j++) {
                SampleInfo *si = &mod.samples[j];
                if (si->length == 0u) continue;
                if (si->length > 512u && !SI_HAS_LOOP(si)) {
                    uint32_t old_cost = (uint32_t)si->length;
                    uint32_t new_cost = ((uint32_t)si->length + 1u) / 2u;
                    needed = needed - old_cost + new_cost;
                    si->flags = (si->flags & ~0x03u) | SI_STYPE_ADPCM;		    
                }
            }
        }

        /* Step B: if we still exceed RAM, downsample samples further.
         *
         * Both ADPCM and PCM samples are candidates -- downsampling happens
         * before encoding so it is always valid.  The only restriction is
         * looped samples cannot use ADPCM (already handled in Step A).
         *
         * Sort key: looped PCM first (can't ADPCM, quality suffers most),
         * then non-looped ADPCM (already cheap; downsampling hurts less),
         * within each tier by length descending (most bytes saved first).
         *
         * Three passes: DS×2, DS×4, DS×8.  Cost formula per sample:
         *   ADPCM: (length / ds_factor + 1) / 2
         *   PCM:    length / ds_factor */
        if (needed > (uint32_t)POKEYMAX_RAM_SIZE) {
            uint8_t  cand[MOD_MAX_SAMPLES];
            uint8_t  ncand = 0u;
            uint8_t  c, k, pass;

            /* Build sorted candidate list (all samples with length>0) */
            for (j = 1u; j <= MOD_MAX_SAMPLES; j++) {
                SampleInfo *sj = &mod.samples[j];
                if (sj->length == 0u) continue;

                /* Sort key: looped-PCM > non-looped-ADPCM, then length desc.
                 * Looped PCM: has_loop && !is_adpcm
                 * Non-looped ADPCM: !has_loop && is_adpcm  (is_adpcm => !has_loop) */
                k = ncand;
                while (k > 0u) {
                    SampleInfo *sk = &mod.samples[cand[k - 1u]];
                    uint8_t sk_looped_pcm = (SI_HAS_LOOP(sk) && !SI_IS_ADPCM(sk)) ? 1u : 0u;
                    uint8_t sj_looped_pcm = (SI_HAS_LOOP(sj) && !SI_IS_ADPCM(sj)) ? 1u : 0u;
                    uint8_t sk_wins = 0u;
                    if (sk_looped_pcm && !sj_looped_pcm) sk_wins = 1u;
                    else if (sk_looped_pcm == sj_looped_pcm && sk->length >= sj->length) sk_wins = 1u;
                    if (sk_wins) break;
                    cand[k] = cand[k - 1u];
                    k--;
                }
                cand[k] = (uint8_t)j;
                ncand++;
            }

            /* Three sweep passes: DS×2, DS×4, DS×8 */
            for (pass = 1u; pass <= 3u && needed > (uint32_t)POKEYMAX_RAM_SIZE; pass++) {
                for (c = 0u; c < ncand && needed > (uint32_t)POKEYMAX_RAM_SIZE; c++) {
                    SampleInfo *sc = &mod.samples[cand[c]];
                    uint8_t target_factor = (uint8_t)(pass == 1u ? 2u : pass == 2u ? 4u : 8u);
                    uint8_t ds_shift = SI_DS_SHIFT(sc);
                    uint8_t ds_factor = (uint8_t)(1u << ds_shift);  /* 1, 2, 4, or 8 */

                    if (ds_factor >= target_factor) continue;

                    {
                        uint32_t ds_old = (uint32_t)ds_factor;
                        uint32_t ds_new = (uint32_t)target_factor;
                        uint32_t old_cost, new_cost;
                        uint8_t ds_shift;
                        if (SI_IS_ADPCM(sc)) {
                            old_cost = ((uint32_t)sc->length / ds_old + 1u) / 2u;
                            new_cost = ((uint32_t)sc->length / ds_new + 1u) / 2u;
                        } else {
                            old_cost = (uint32_t)sc->length / ds_old;
                            new_cost = (uint32_t)sc->length / ds_new;
                        }
                        needed = needed - old_cost + new_cost;
                        // target_factor is 2, 4, or 8 — convert to shift
                        ds_shift = (target_factor == 2u) ? 1u : (target_factor == 4u) ? 2u : 3u;
                        sc->flags = (sc->flags & ~0x0Cu) | (uint8_t)(ds_shift << 2);			
                    }
                }
            }

            if (needed > (uint32_t)POKEYMAX_RAM_SIZE) {
                printf("Warn: samples need %lu, RAM %u\n",
                       (unsigned long)needed, (unsigned)POKEYMAX_RAM_SIZE);
            }
        }
    }
    /* Pass 1 complete: mod.samples[i].downsample_factor and is_adpcm/is_8bit
     * are now set for every sample. The load loop below just executes the plan. */
    {
        /* Print up to 4 lines (<=40 chars) summarising Pass 1 decisions.
         * ADPCM+DS: effective ratio = ds_factor*2 (/2 /4 /8 /16)
         * PCM+DS:   effective ratio = ds_factor   (/1 /2 /4 /8)
         * Format:
         *   ADPCM/2:N /4:N /8:N /16:N =Xb
         *   PCM/1:N /2:N /4:N /8:N =Xb
         *   [OVER by Xb!]
         *   Total:Xb / Yb
         */
        uint8_t  j;
        /* ADPCM counts: index 0=/2 1=/4 2=/8 3=/16 (ds 1/2/4/8) */
        uint8_t  na[4] = {0u,0u,0u,0u};
        /* PCM counts:   index 0=/1 1=/2 2=/4 3=/8  (ds 1/2/4/8) */
        uint8_t  np[4] = {0u,0u,0u,0u};
        uint32_t abytes = 0u, pbytes = 0u;
        for (j = 1u; j <= MOD_MAX_SAMPLES; j++) {
            uint8_t fi;
            SampleInfo *si = &mod.samples[j];
            if (si->length == 0u) continue;
            fi = SI_DS_SHIFT(si);
            if (SI_IS_ADPCM(si)) {
                na[fi]++;
		abytes += ((uint32_t)(si->length >> fi) + 1u) / 2u;
            } else {
                np[fi]++;
                pbytes += (uint32_t)si->length >> fi;
            }
        }
        printf("ADPCM/ 2:%u 4:%u 8:%u 16:%u =%lub\n",
               (unsigned)na[0],(unsigned)na[1],
               (unsigned)na[2],(unsigned)na[3],
               (unsigned long)abytes);
        printf("PCM  / 1:%u 2:%u 4:%u  8:%u =%lub\n",
               (unsigned)np[0],(unsigned)np[1],
               (unsigned)np[2],(unsigned)np[3],
               (unsigned long)pbytes);
        {
            uint32_t total = abytes + pbytes;
            if (total > (uint32_t)POKEYMAX_RAM_SIZE)
                printf("OVER by %lub!\n",
                       (unsigned long)(total-(uint32_t)POKEYMAX_RAM_SIZE));
            printf("Total:%lub / %ub\n",
                   (unsigned long)total,(unsigned)POKEYMAX_RAM_SIZE);
        }
    }

    sample_data_offset = mod.pattern_data_offset
                       + mod.pattern_data_size;
    if (fseek(mod_file, (long)sample_data_offset, SEEK_SET) != 0) return 1;

    pokeymax_init();
    loaded_samples      = 0;
    loaded_source_bytes = 0;
    loaded_stored_bytes = 0;
    progress_started    = 0;
    had_status_output   = 0;

    for (i = 1; i <= MOD_MAX_SAMPLES; i++)
    {
        SampleInfo *si = &mod.samples[i];
        uint16_t    ram_addr, ram_needed;
        uint16_t    remaining, chunk, written, out_len;
        uint8_t ds_shift;
        uint8_t ds_factor;
        ADPCMState  adpcm_state;

        if (si->length == 0u) continue;

        if (!progress_started) {
            if (s_progress_plugin && s_progress_plugin->begin) {
                s_progress_plugin->begin(s_progress_plugin->ctx,
                                         total_sample_bytes);
            }
            progress_started = 1;
        }

        /* Compression plan already set by Pass 1 (is_adpcm, is_8bit,
         * downsample_factor).  Just compute ram_needed from it. */
        ds_shift = SI_DS_SHIFT(si);
        ds_factor = (uint8_t)(1u << ds_shift);  /* 1, 2, 4, or 8 */
        if (SI_IS_ADPCM(si)) {
            ram_needed = ((si->length >> ds_shift) + 1u) / 2u;
        } else {
            ram_needed = (si->length + (uint16_t)((1u << ds_shift) - 1u)) >> ds_shift;
        }

        ram_addr = pokeymax_alloc(ram_needed);

        if (ram_addr == POKEYMAX_ALLOC_FAIL) {
            if (s_progress_plugin && s_progress_plugin->update) {
                s_progress_plugin->update(s_progress_plugin->ctx,
                                          si,
                                          (uint16_t)(loaded_samples + 1u),
                                          total_samples_to_load,
                                          loaded_source_bytes,
                                          total_sample_bytes,
                                          loaded_stored_bytes,
                                          1u);
            }
            had_status_output = 1;
            fseek(mod_file, (long)si->length, SEEK_CUR);
            si->length = 0;
            continue;
        }

        si->pokeymax_addr = ram_addr;
        si->pokeymax_len  = ram_needed;

        /* Adjust loop points for downsampled samples */
        if (ds_shift > 0u) {
            si->loop_start >>= ds_shift;
            si->loop_len   >>= ds_shift;
            if (si->loop_len < 2u && SI_HAS_LOOP(si)) si->loop_len = 2u;
        }

        adpcm_state.predictor  = 0;
        adpcm_state.step_index = 0;
        remaining = si->length;
        written   = 0;

        while (remaining > 0u) {
            chunk = (remaining > SECTOR_SIZE) ? SECTOR_SIZE : remaining;
            if (fread(sector_buf, 1, chunk, mod_file) != chunk) return 1;
            if (SI_IS_ADPCM(si)) {
                /* Optionally downsample first, then ADPCM encode.
                 * adpcm_out (128B) holds the decimated PCM intermediate.
                 * enc_buf   (128B) holds the final ADPCM output.
                 * Decimated size <= SECTOR_SIZE/2 = 128B, always fits. */
                static uint8_t enc_buf[SECTOR_SIZE / 2];
                const int8_t *src_ptr;
                uint16_t      src_len;
                if (ds_factor > 1u) {
                    uint16_t k, di = 0u;
                    uint8_t  factor = ds_factor;
                    for (k = 0u; k < chunk; k += factor)
                        adpcm_out[di++] = sector_buf[k];
                    src_ptr = (const int8_t*)adpcm_out;
                    src_len = di;
                } else {
                    src_ptr = (const int8_t*)sector_buf;
                    src_len = chunk;
                }
                out_len = adpcm_encode_block(src_ptr, src_len,
                                             enc_buf, &adpcm_state);
                pokeymax_write_ram(ram_addr + written, enc_buf, out_len);
                written += out_len;
            } else if (ds_factor > 1u) {
                /* Downsample: pick every Nth byte into adpcm_out (reuse as temp buf) */
                uint16_t k, out_idx = 0;
                uint8_t  factor = ds_factor;
                for (k = 0u; k < chunk; k += factor) {
                    adpcm_out[out_idx++] = sector_buf[k];
                }
                pokeymax_write_ram(ram_addr + written, adpcm_out, out_idx);
                written += out_idx;
            } else {
                pokeymax_write_ram(ram_addr + written, sector_buf, chunk);
                written += chunk;
            }
            remaining -= chunk;

            if (s_progress_plugin && s_progress_plugin->update) {
                s_progress_plugin->update(s_progress_plugin->ctx,
                                          si,
                                          (uint16_t)(loaded_samples + 1u),
                                          total_samples_to_load,
                                          (uint32_t)(loaded_source_bytes + (si->length - remaining)),
                                          total_sample_bytes,
                                          (uint32_t)(loaded_stored_bytes + written),
                                          0u);
            }
            had_status_output = 1;
        }

        loaded_samples++;
        loaded_source_bytes += si->length;  /* source length, before any override */
        /* Update si->length to reflect the actual stored size:
         * - downsampled PCM: stored = length / downsample_factor
         * - ADPCM (any ds): stored = pokeymax_len (already computed correctly)
         * Always use pokeymax_len as the authoritative stored size. */
        si->length = si->pokeymax_len;
        loaded_stored_bytes += si->pokeymax_len;
    }

    if (had_status_output) {
        if (s_progress_plugin && s_progress_plugin->end) {
            s_progress_plugin->end(s_progress_plugin->ctx);
        }
    }

    if (mod_file) { fclose(mod_file); mod_file = NULL; }

    mod.order_pos     = 0;
    mod.row           = 0;
    mod.tick          = 0;
    mod.speed         = DEFAULT_SPEED;
    mod.bpm           = DEFAULT_BPM;
    mod.loop_song     = 1;
    mod.playing       = 0;
    mod.pattern_break = 0;
    mod.do_jump       = 0;
    mod.pattern_delay = 0;

    return 0;
}
